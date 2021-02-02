/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Xdr_h
#define vm_Xdr_h

#include "mozilla/EndianUtils.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/Utf8.h"

#include <type_traits>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "NamespaceImports.h"

#include "frontend/ParserAtom.h"
#include "js/CompileOptions.h"
#include "js/Transcoding.h"
#include "js/TypeDecls.h"
#include "vm/JSAtom.h"

namespace js {

struct SourceExtent;

namespace frontend {
struct CompilationStencil;
struct CompilationStencilSet;
struct CompilationInput;
struct BaseCompilationStencil;
}  // namespace frontend

class LifoAlloc;

enum XDRMode { XDR_ENCODE, XDR_DECODE };

template <typename T>
using XDRResultT = mozilla::Result<T, JS::TranscodeResult>;
using XDRResult = XDRResultT<mozilla::Ok>;

using XDRAtomTable = JS::GCVector<PreBarriered<JSAtom*>>;
using XDRAtomMap = JS::GCHashMap<PreBarriered<JSAtom*>, uint32_t>;

class XDRBufferBase {
 public:
  explicit XDRBufferBase(JSContext* cx, size_t cursor = 0)
      : context_(cx), cursor_(cursor) {}

  JSContext* cx() const { return context_; }

  size_t cursor() const { return cursor_; }

 protected:
  JSContext* const context_;
  size_t cursor_;

  friend class XDRIncrementalStencilEncoder;
};

template <XDRMode mode>
class XDRBuffer;

template <>
class XDRBuffer<XDR_ENCODE> : public XDRBufferBase {
 public:
  XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor), buffer_(buffer) {}

  uint8_t* write(size_t n) {
    MOZ_ASSERT(n != 0);
    if (!buffer_.growByUninitialized(n)) {
      ReportOutOfMemory(cx());
      return nullptr;
    }
    uint8_t* ptr = &buffer_[cursor_];
    cursor_ += n;
    return ptr;
  }

  bool align32() {
    size_t extra = cursor_ % 4;
    if (extra) {
      size_t padding = 4 - extra;
      if (!buffer_.appendN(0, padding)) {
        ReportOutOfMemory(cx());
        return false;
      }
      cursor_ += padding;
    }
    return true;
  }

#ifdef DEBUG
  bool isAligned32() { return cursor_ % 4 == 0; }
#endif

  const uint8_t* read(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

  const uint8_t* peek(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

 private:
  JS::TranscodeBuffer& buffer_;
};

template <>
class XDRBuffer<XDR_DECODE> : public XDRBufferBase {
 public:
  XDRBuffer(JSContext* cx, const JS::TranscodeRange& range)
      : XDRBufferBase(cx), buffer_(range) {}

  XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor), buffer_(buffer.begin(), buffer.length()) {}

  bool align32() {
    size_t extra = cursor_ % 4;
    if (extra) {
      size_t padding = 4 - extra;
      cursor_ += padding;

      // Don't let buggy code read past our buffer
      if (cursor_ > buffer_.length()) {
        return false;
      }
    }
    return true;
  }

#ifdef DEBUG
  bool isAligned32() { return cursor_ % 4 == 0; }
#endif

  const uint8_t* read(size_t n) {
    MOZ_ASSERT(cursor_ < buffer_.length());
    uint8_t* ptr = &buffer_[cursor_];
    cursor_ += n;

    // Don't let buggy code read past our buffer
    if (cursor_ > buffer_.length()) {
      return nullptr;
    }

    return ptr;
  }

  const uint8_t* peek(size_t n) {
    MOZ_ASSERT(cursor_ < buffer_.length());
    uint8_t* ptr = &buffer_[cursor_];

    // Don't let buggy code read past our buffer
    if (cursor_ + n > buffer_.length()) {
      return nullptr;
    }

    return ptr;
  }

  uint8_t* write(size_t n) {
    MOZ_CRASH("Should never write in decode mode");
    return nullptr;
  }

 private:
  const JS::TranscodeRange buffer_;
};

class XDRCoderBase;
class XDRIncrementalEncoder;

// An AutoXDRTree is used to identify section encoded by an
// XDRIncrementalEncoder.
//
// Its primary goal is to identify functions, such that we can first encode them
// as a lazy BaseScript, and later replaced by them by their corresponding
// bytecode once delazified.
//
// As a convenience, this is also used to identify the top-level of the content
// encoded by an XDRIncrementalEncoder.
//
// Sections can be encoded any number of times in an XDRIncrementalEncoder, and
// the latest encoded version would replace all the previous one.
class MOZ_RAII AutoXDRTree {
 public:
  // For a JSFunction, a tree key is defined as being:
  //     script()->begin << 32 | script()->end
  //
  // Based on the invariant that |begin <= end|, we can make special
  // keys, such as the top-level script.
  using Key = uint64_t;

  AutoXDRTree(XDRCoderBase* xdr, Key key);
  ~AutoXDRTree();

  // Indicate the lack of a key for the current tree.
  static constexpr Key noKey = 0;

  // Used to end the slices when there is no children.
  static constexpr Key noSubTree = Key(1) << 32;

  // Used as the root key of the tree in the hash map.
  static constexpr Key topLevel = Key(2) << 32;

 private:
  friend class XDRIncrementalEncoder;

  Key key_;
  AutoXDRTree* parent_;
  XDRCoderBase* xdr_;
};

template <typename CharT>
using XDRTranscodeString =
    mozilla::MaybeOneOf<const CharT*, js::UniquePtr<CharT[], JS::FreePolicy>>;

class XDRCoderBase {
 private:
#ifdef DEBUG
  JS::TranscodeResult resultCode_;
#endif

 protected:
  XDRCoderBase()
#ifdef DEBUG
      : resultCode_(JS::TranscodeResult_Ok)
#endif
  {
  }

 public:
  virtual AutoXDRTree::Key getTopLevelTreeKey() const {
    return AutoXDRTree::noKey;
  }
  virtual AutoXDRTree::Key getTreeKey(JSFunction* fun) const {
    return AutoXDRTree::noKey;
  }
  virtual void createOrReplaceSubTree(AutoXDRTree* child){};
  virtual void endSubTree(){};

#ifdef DEBUG
  // Record logical failures of XDR.
  JS::TranscodeResult resultCode() const { return resultCode_; }
  void setResultCode(JS::TranscodeResult code) {
    MOZ_ASSERT(resultCode() == JS::TranscodeResult_Ok);
    resultCode_ = code;
  }
  bool validateResultCode(JSContext* cx, JS::TranscodeResult code) const;
#endif
};

/*
 * XDR serialization state.  All data is encoded in little endian.
 */
template <XDRMode mode>
class XDRState : public XDRCoderBase {
 protected:
  XDRBuffer<mode> mainBuf;
  XDRBuffer<mode>* buf;

 public:
  XDRState(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : mainBuf(cx, buffer, cursor), buf(&mainBuf) {}

  template <typename RangeType>
  XDRState(JSContext* cx, const RangeType& range)
      : mainBuf(cx, range), buf(&mainBuf) {}

  // No default copy constructor or copying assignment, because |buf|
  // is an internal pointer.
  XDRState(const XDRState&) = delete;
  XDRState& operator=(const XDRState&) = delete;

  virtual ~XDRState() = default;

  JSContext* cx() const { return mainBuf.cx(); }
  virtual bool isForStencil() const { return false; }
  virtual XDRResultT<bool> checkAlreadyCoded(
      const frontend::BaseCompilationStencil& stencil) {
    return false;
  }

  virtual bool isMultiDecode() const { return false; }

  virtual bool hasOptions() const { return false; }
  virtual const JS::ReadOnlyCompileOptions& options() {
    MOZ_CRASH("does not have options");
  }
  virtual bool hasScriptSourceObjectOut() const { return false; }
  virtual ScriptSourceObject** scriptSourceObjectOut() {
    MOZ_CRASH("does not have scriptSourceObjectOut.");
  }

  virtual bool hasAtomMap() const { return false; }
  virtual XDRAtomMap& atomMap() { MOZ_CRASH("does not have atomMap"); }
  virtual uint32_t& natoms() { MOZ_CRASH("does not have atomMap."); }

  // The number of chunks (BaseCompilationStencils) in the buffer.
  virtual uint32_t& nchunks() { MOZ_CRASH("does not have atomMap."); }

  virtual bool hasAtomTable() const { return false; }
  virtual XDRAtomTable& atomTable() { MOZ_CRASH("does not have atomTable"); }
  virtual frontend::ParserAtomSpanBuilder& frontendAtoms() {
    MOZ_CRASH("does not have frontendAtoms");
  }
  virtual LifoAlloc& stencilAlloc() { MOZ_CRASH("does not have stencilAlloc"); }
  virtual void finishAtomTable() { MOZ_CRASH("does not have atomTable"); }

  virtual bool isMainBuf() { return true; }

  virtual void switchToAtomBuf() { MOZ_CRASH("cannot switch to atom buffer."); }
  virtual void switchToMainBuf() { MOZ_CRASH("cannot switch to main buffer."); }
  virtual void switchToHeaderBuf() {
    MOZ_CRASH("cannot switch to header buffer.");
  }

  virtual XDRResult codeDelazificationStencils(
      frontend::CompilationStencilSet& stencilSet) {
    MOZ_CRASH("cannot code delazification stencils.");
  }

  template <typename T = mozilla::Ok>
  XDRResultT<T> fail(JS::TranscodeResult code) {
#ifdef DEBUG
    MOZ_ASSERT(code != JS::TranscodeResult_Ok);
    MOZ_ASSERT(validateResultCode(cx(), code));
    setResultCode(code);
#endif
    return mozilla::Err(code);
  }

  XDRResult align32() {
    if (!buf->align32()) {
      return fail(JS::TranscodeResult_Throw);
    }
    return Ok();
  }

#ifdef DEBUG
  bool isAligned32() { return buf->isAligned32(); }
#endif

  XDRResult readData(const uint8_t** pptr, size_t length) {
    const uint8_t* ptr = buf->read(length);
    if (!ptr) {
      return fail(JS::TranscodeResult_Failure_BadDecode);
    }
    *pptr = ptr;
    return Ok();
  }

  // Peek the `sizeof(T)` bytes and return the pointer to `*pptr`.
  // The caller is responsible for aligning the buffer by calling `align32`.
  template <typename T>
  XDRResult peekData(const T** pptr) {
    static_assert(alignof(T) <= 4);
    MOZ_ASSERT(isAligned32());
    const uint8_t* ptr = buf->peek(sizeof(T));
    if (!ptr) {
      return fail(JS::TranscodeResult_Failure_BadDecode);
    }
    *pptr = reinterpret_cast<const T*>(ptr);
    return Ok();
  }

  XDRResult codeUint8(uint8_t* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Throw);
      }
      *ptr = *n;
    } else {
      const uint8_t* ptr = buf->read(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Failure_BadDecode);
      }
      *n = *ptr;
    }
    return Ok();
  }

  XDRResult codeUint16(uint16_t* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Throw);
      }
      mozilla::LittleEndian::writeUint16(ptr, *n);
    } else {
      const uint8_t* ptr = buf->read(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Failure_BadDecode);
      }
      *n = mozilla::LittleEndian::readUint16(ptr);
    }
    return Ok();
  }

  XDRResult codeUint32(uint32_t* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Throw);
      }
      mozilla::LittleEndian::writeUint32(ptr, *n);
    } else {
      const uint8_t* ptr = buf->read(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Failure_BadDecode);
      }
      *n = mozilla::LittleEndian::readUint32(ptr);
    }
    return Ok();
  }

  XDRResult codeUint64(uint64_t* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Throw);
      }
      mozilla::LittleEndian::writeUint64(ptr, *n);
    } else {
      const uint8_t* ptr = buf->read(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult_Failure_BadDecode);
      }
      *n = mozilla::LittleEndian::readUint64(ptr);
    }
    return Ok();
  }

  /*
   * Use SFINAE to refuse any specialization which is not an enum.  Uses of
   * this function do not have to specialize the type of the enumerated field
   * as C++ will extract the parameterized from the argument list.
   */
  template <typename T>
  XDRResult codeEnum32(T* val, std::enable_if_t<std::is_enum_v<T>>* = nullptr) {
    // Mix the enumeration value with a random magic number, such that a
    // corruption with a low-ranged value (like 0) is less likely to cause a
    // miss-interpretation of the XDR content and instead cause a failure.
    const uint32_t MAGIC = 0x21AB218C;
    uint32_t tmp;
    if (mode == XDR_ENCODE) {
      tmp = uint32_t(*val) ^ MAGIC;
    }
    MOZ_TRY(codeUint32(&tmp));
    if (mode == XDR_DECODE) {
      *val = T(tmp ^ MAGIC);
    }
    return Ok();
  }

  XDRResult codeDouble(double* dp) {
    union DoublePun {
      double d;
      uint64_t u;
    } pun;
    if (mode == XDR_ENCODE) {
      pun.d = *dp;
    }
    MOZ_TRY(codeUint64(&pun.u));
    if (mode == XDR_DECODE) {
      *dp = pun.d;
    }
    return Ok();
  }

  XDRResult codeMarker(uint32_t magic) {
    uint32_t actual = magic;
    MOZ_TRY(codeUint32(&actual));
    if (actual != magic) {
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR marker");
      return fail(JS::TranscodeResult_Failure_BadDecode);
    }
    return Ok();
  }

  XDRResult codeBytes(void* bytes, size_t len) {
    if (len == 0) {
      return Ok();
    }
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(len);
      if (!ptr) {
        return fail(JS::TranscodeResult_Throw);
      }
      memcpy(ptr, bytes, len);
    } else {
      const uint8_t* ptr = buf->read(len);
      if (!ptr) {
        return fail(JS::TranscodeResult_Failure_BadDecode);
      }
      memcpy(bytes, ptr, len);
    }
    return Ok();
  }

  // While encoding, code the given data to the buffer.
  // While decoding, borrow the buffer and return it to `*data`.
  //
  // The data can have extra bytes after `sizeof(T)`, and the caller should
  // provide the entire data length as `length`.
  //
  // The caller is responsible for aligning the buffer by calling `align32`.
  template <typename T>
  XDRResult borrowedData(T** data, uint32_t length) {
    static_assert(alignof(T) <= 4);
    MOZ_ASSERT(isAligned32());

    if (mode == XDR_ENCODE) {
      MOZ_TRY(codeBytes(*data, length));
    } else {
      const uint8_t* cursor = nullptr;
      MOZ_TRY(readData(&cursor, length));
      *data = reinterpret_cast<T*>(const_cast<uint8_t*>(cursor));
    }
    return Ok();
  }

  // Prefer using a variant below that is encoding aware.
  XDRResult codeChars(char* chars, size_t nchars);

  XDRResult codeChars(JS::Latin1Char* chars, size_t nchars);
  XDRResult codeChars(mozilla::Utf8Unit* units, size_t nchars);
  XDRResult codeChars(char16_t* chars, size_t nchars);

  // Transcode null-terminated strings. When decoding, a new buffer is
  // allocated and ownership is returned to caller.
  //
  // NOTE: Throws if string longer than JSString::MAX_LENGTH.
  XDRResult codeCharsZ(XDRTranscodeString<char>& buffer);
  XDRResult codeCharsZ(XDRTranscodeString<char16_t>& buffer);

  XDRResult codeModuleObject(MutableHandleModuleObject modp);
  XDRResult codeFunction(JS::MutableHandleFunction objp,
                         HandleScriptSourceObject sourceObject = nullptr);
  XDRResult codeScript(MutableHandleScript scriptp);
  XDRResult codeStencil(frontend::CompilationStencil& stencil);
  XDRResult codeFunctionStencil(frontend::BaseCompilationStencil& stencil);
};

using XDREncoder = XDRState<XDR_ENCODE>;
using XDRDecoderBase = XDRState<XDR_DECODE>;

class XDRDecoder : public XDRDecoderBase {
 public:
  XDRDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
             JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRDecoderBase(cx, buffer, cursor), options_(options), atomTable_(cx) {
    MOZ_ASSERT(options);
  }

  template <typename RangeType>
  XDRDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
             const RangeType& range)
      : XDRDecoderBase(cx, range), options_(options), atomTable_(cx) {
    MOZ_ASSERT(options);
  }

  bool hasAtomTable() const override { return hasFinishedAtomTable_; }
  XDRAtomTable& atomTable() override { return atomTable_; }
  void finishAtomTable() override { hasFinishedAtomTable_ = true; }

  bool hasOptions() const override { return true; }
  const JS::ReadOnlyCompileOptions& options() override { return *options_; }

  void trace(JSTracer* trc);

 private:
  const JS::ReadOnlyCompileOptions* options_;
  XDRAtomTable atomTable_;
  bool hasFinishedAtomTable_ = false;
};

/*
 * The stencil decoder accepts `options` and `range` as input, along
 * with a freshly initialized `parserAtoms` table.
 *
 * The decoded stencils are outputted to the default-initialized
 * `stencil` parameter of `codeStencil` method, and decoded atoms are
 * interned into the `parserAtoms` parameter of the ctor.
 *
 * The decoded stencils borrow the input `buffer`/`range`, and the consumer
 * has to keep the buffer alive while the decoded stencils are alive.
 */
class XDRStencilDecoder : public XDRDecoderBase {
  uint32_t nchunks_ = 0;

 public:
  XDRStencilDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
                    JS::TranscodeBuffer& buffer, size_t cursor)
      : XDRDecoderBase(cx, buffer, cursor), options_(options) {
    MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(buffer.begin()));
    MOZ_ASSERT(JS::IsTranscodingBytecodeOffsetAligned(cursor));
    MOZ_ASSERT(options_);
  }

  XDRStencilDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
                    const JS::TranscodeRange& range)
      : XDRDecoderBase(cx, range), options_(options) {
    MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(range.begin().get()));
    MOZ_ASSERT(options_);
  }

  uint32_t& nchunks() override { return nchunks_; }

  bool isForStencil() const override { return true; }

  bool hasAtomTable() const override { return hasFinishedAtomTable_; }
  frontend::ParserAtomSpanBuilder& frontendAtoms() override {
    return *parserAtomBuilder_;
  }
  LifoAlloc& stencilAlloc() override { return *stencilAlloc_; }
  void finishAtomTable() override { hasFinishedAtomTable_ = true; }

  bool hasOptions() const override { return true; }
  const JS::ReadOnlyCompileOptions& options() override { return *options_; }

  XDRResult codeStencils(frontend::CompilationStencilSet& stencilSet);

 private:
  const JS::ReadOnlyCompileOptions* options_;
  bool hasFinishedAtomTable_ = false;
  frontend::ParserAtomSpanBuilder* parserAtomBuilder_ = nullptr;
  LifoAlloc* stencilAlloc_ = nullptr;
};

class XDROffThreadDecoder : public XDRDecoder {
  ScriptSourceObject** sourceObjectOut_;
  bool isMultiDecode_;

 public:
  enum class Type {
    Single,
    Multi,
  };

  // Note, when providing an JSContext, where isJSContext is false,
  // then the initialization of the ScriptSourceObject would remain
  // incomplete. Thus, the sourceObjectOut must be used to finish the
  // initialization with ScriptSourceObject::initFromOptions after the
  // decoding.
  //
  // When providing a sourceObjectOut pointer, you have to ensure that it is
  // marked by the GC to avoid dangling pointers.
  XDROffThreadDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
                      Type type, ScriptSourceObject** sourceObjectOut,
                      const JS::TranscodeRange& range)
      : XDRDecoder(cx, options, range),
        sourceObjectOut_(sourceObjectOut),
        isMultiDecode_(type == Type::Multi) {
    MOZ_ASSERT(sourceObjectOut);
    MOZ_ASSERT(*sourceObjectOut == nullptr);
  }

  bool isMultiDecode() const override { return isMultiDecode_; }

  bool hasScriptSourceObjectOut() const override { return true; }
  ScriptSourceObject** scriptSourceObjectOut() override {
    return sourceObjectOut_;
  }
};

class XDRIncrementalEncoderBase : public XDREncoder {
 protected:
  JS::TranscodeBuffer slices_;

 public:
  explicit XDRIncrementalEncoderBase(JSContext* cx)
      : XDREncoder(cx, slices_, 0) {}

  void switchToBuffer(XDRBuffer<XDR_ENCODE>* target) { buf = target; }

  bool isMainBuf() override { return buf == &mainBuf; }

  // Switch to streaming into the main buffer.
  void switchToMainBuf() override { switchToBuffer(&mainBuf); }

  virtual XDRResult linearize(JS::TranscodeBuffer& buffer,
                              js::ScriptSource* ss) {
    MOZ_CRASH("cannot linearize.");
  }

  virtual void trace(JSTracer* trc) {}
};

class XDRIncrementalEncoder : public XDRIncrementalEncoderBase {
  // The incremental encoder encodes the content of scripts and functions in
  // the XDRBuffer. It can be used to encode multiple times the same
  // AutoXDRTree, and uses its key to identify which part to replace.
  //
  // Internally, this encoder keeps a tree representation of the scopes. Each
  // node is composed of a vector of slices which are interleaved by child
  // nodes.
  //
  // A slice corresponds to an index and a length within the content of the
  // slices_ buffer. The index is updated when a slice is created, and the
  // length is updated when the slice is ended, either by creating a new scope
  // child, or by closing the scope and going back to the parent.
  //
  //                  +---+---+---+
  //        begin     |   |   |   |
  //        length    |   |   |   |
  //        child     | . | . | . |
  //                  +-|-+-|-+---+
  //                    |   |
  //          +---------+   +---------+
  //          |                       |
  //          v                       v
  //      +---+---+                 +---+
  //      |   |   |                 |   |
  //      |   |   |                 |   |
  //      | . | . |                 | . |
  //      +-|-+---+                 +---+
  //        |
  //        |
  //        |
  //        v
  //      +---+
  //      |   |
  //      |   |
  //      | . |
  //      +---+
  //
  //
  // The tree key is used to identify the child nodes, and to make them
  // easily replaceable.
  //
  // The tree is rooted at the |topLevel| key.
  //

  struct Slice {
    size_t sliceBegin;
    size_t sliceLength;
    AutoXDRTree::Key child;
  };

  using SlicesNode = Vector<Slice, 1, SystemAllocPolicy>;
  using SlicesTree =
      HashMap<AutoXDRTree::Key, SlicesNode, DefaultHasher<AutoXDRTree::Key>,
              SystemAllocPolicy>;

  // Header buffer.
  JS::TranscodeBuffer header_;
  XDRBuffer<XDR_ENCODE> headerBuf_;

  // Atom buffer.
  JS::TranscodeBuffer atoms_;
  XDRBuffer<XDR_ENCODE> atomBuf_;

  uint32_t natoms_ = 0;

  // Last opened XDR-tree on the stack.
  AutoXDRTree* scope_;
  // Node corresponding to the opened scope.
  SlicesNode* node_;
  // Tree of slices.
  SlicesTree tree_;
  // Map from atoms to their index in the atom buffer
  XDRAtomMap atomMap_;
  bool oom_;

  class DepthFirstSliceIterator;

 public:
  explicit XDRIncrementalEncoder(JSContext* cx)
      : XDRIncrementalEncoderBase(cx),
        headerBuf_(cx, header_, 0),
        atomBuf_(cx, atoms_, 0),
        scope_(nullptr),
        node_(nullptr),
        atomMap_(cx),
        oom_(false) {}

  virtual ~XDRIncrementalEncoder() = default;

  uint32_t& natoms() override { return natoms_; }

  // Switch from streaming into the main buffer into the atom buffer.
  void switchToAtomBuf() override { switchToBuffer(&atomBuf_); }

  // Switch to streaming into the header buffer.
  void switchToHeaderBuf() override { switchToBuffer(&headerBuf_); }

  bool hasAtomMap() const override { return true; }
  XDRAtomMap& atomMap() override { return atomMap_; }

  AutoXDRTree::Key getTopLevelTreeKey() const override;
  AutoXDRTree::Key getTreeKey(JSFunction* fun) const override;

  void createOrReplaceSubTree(AutoXDRTree* child) override;
  void endSubTree() override;

  // Append the content collected during the incremental encoding into the
  // buffer given as argument.
  XDRResult linearize(JS::TranscodeBuffer& buffer,
                      js::ScriptSource* ss) override;

  void trace(JSTracer* trc) override;
};

class XDRIncrementalStencilEncoder : public XDRIncrementalEncoderBase {
  // The structure of the resulting buffer is:
  //
  // 1. Header
  //   a. Version
  //   b. ScriptSource
  //   c. Chunk count
  //   d. Alignment padding
  // 2. Initial Chunk
  //   a. ParseAtomTable
  //   b. BaseCompilationStencil
  //   c. ScriptStencilExtra[]
  //   d. StencilModuleMetadata (if exists)
  // 3. Array of Delazification Chunks
  //   a. ParseAtomTable
  //   b. BaseCompilationStencil

  // A set of functions that is passed to codeFunctionStencil.
  // Used to avoid encoding delazification for same function twice.
  // NOTE: This is not a set of all encoded functions.
  using FunctionKey = uint32_t;
  HashSet<FunctionKey> encodedFunctions_;

 public:
  explicit XDRIncrementalStencilEncoder(JSContext* cx)
      : XDRIncrementalEncoderBase(cx), encodedFunctions_(cx) {}

  virtual ~XDRIncrementalStencilEncoder() = default;

  XDRResultT<bool> checkAlreadyCoded(
      const frontend::BaseCompilationStencil& stencil) override;

  bool isForStencil() const override { return true; }

  XDRResult linearize(JS::TranscodeBuffer& buffer,
                      js::ScriptSource* ss) override;

  XDRResult codeStencils(frontend::CompilationStencilSet& stencilSet);
};

template <XDRMode mode>
XDRResult XDRAtomOrNull(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

template <XDRMode mode>
XDRResult XDRAtom(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

template <XDRMode mode>
XDRResult XDRAtomData(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

template <XDRMode mode>
XDRResult XDRParserAtomEntry(XDRState<mode>* xdr,
                             frontend::ParserAtomEntry** atomp);

template <XDRMode mode>
XDRResult XDRBaseCompilationStencil(XDRState<mode>* xdr,
                                    frontend::BaseCompilationStencil& stencil);

template <XDRMode mode>
XDRResult XDRCompilationStencil(XDRState<mode>* xdr,
                                frontend::CompilationStencil& stencil);

} /* namespace js */

#endif /* vm_Xdr_h */
