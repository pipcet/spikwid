/* clang-format off */
/* -*- Mode: Objective-C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* clang-format on */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DocAccessibleParent.h"
#include "AccessibleOrProxy.h"
#include "nsCocoaUtils.h"

#include "mozilla/a11y/DocAccessiblePlatformExtParent.h"

#import "GeckoTextMarker.h"

extern "C" {

CFTypeID AXTextMarkerGetTypeID();

AXTextMarkerRef AXTextMarkerCreate(CFAllocatorRef allocator, const UInt8* bytes,
                                   CFIndex length);

const UInt8* AXTextMarkerGetBytePtr(AXTextMarkerRef text_marker);

size_t AXTextMarkerGetLength(AXTextMarkerRef text_marker);

CFTypeID AXTextMarkerRangeGetTypeID();

AXTextMarkerRangeRef AXTextMarkerRangeCreate(CFAllocatorRef allocator,
                                             AXTextMarkerRef start_marker,
                                             AXTextMarkerRef end_marker);

AXTextMarkerRef AXTextMarkerRangeCopyStartMarker(
    AXTextMarkerRangeRef text_marker_range);

AXTextMarkerRef AXTextMarkerRangeCopyEndMarker(
    AXTextMarkerRangeRef text_marker_range);
}

namespace mozilla {
namespace a11y {

struct OpaqueGeckoTextMarker {
  OpaqueGeckoTextMarker(uintptr_t aDoc, uintptr_t aID, int32_t aOffset)
      : mDoc(aDoc), mID(aID), mOffset(aOffset) {}
  OpaqueGeckoTextMarker() {}
  uintptr_t mDoc;
  uintptr_t mID;
  int32_t mOffset;
};

static bool DocumentExists(AccessibleOrProxy aDoc, uintptr_t aDocPtr) {
  if (aDoc.Bits() == aDocPtr) {
    return true;
  }

  if (aDoc.IsAccessible()) {
    DocAccessible* docAcc = aDoc.AsAccessible()->AsDoc();
    uint32_t docCount = docAcc->ChildDocumentCount();
    for (uint32_t i = 0; i < docCount; i++) {
      if (DocumentExists(docAcc->GetChildDocumentAt(i), aDocPtr)) {
        return true;
      }
    }
  } else {
    DocAccessibleParent* docProxy = aDoc.AsProxy()->AsDoc();
    size_t docCount = docProxy->ChildDocCount();
    for (uint32_t i = 0; i < docCount; i++) {
      if (DocumentExists(docProxy->ChildDocAt(i), aDocPtr)) {
        return true;
      }
    }
  }

  return false;
}

// GeckoTextMarker

GeckoTextMarker::GeckoTextMarker(AccessibleOrProxy aDoc,
                                 AXTextMarkerRef aTextMarker) {
  MOZ_ASSERT(!aDoc.IsNull());
  OpaqueGeckoTextMarker opaqueMarker;
  if (aTextMarker &&
      AXTextMarkerGetLength(aTextMarker) == sizeof(OpaqueGeckoTextMarker)) {
    memcpy(&opaqueMarker, AXTextMarkerGetBytePtr(aTextMarker),
           sizeof(OpaqueGeckoTextMarker));
    if (DocumentExists(aDoc, opaqueMarker.mDoc)) {
      AccessibleOrProxy doc;
      doc.SetBits(opaqueMarker.mDoc);
      if (doc.IsProxy()) {
        mContainer = doc.AsProxy()->AsDoc()->GetAccessible(opaqueMarker.mID);
      } else {
        mContainer = doc.AsAccessible()->AsDoc()->GetAccessibleByUniqueID(
            reinterpret_cast<void*>(opaqueMarker.mID));
      }
    }

    mOffset = opaqueMarker.mOffset;
  }
}

GeckoTextMarker GeckoTextMarker::MarkerFromIndex(const AccessibleOrProxy& aRoot,
                                                 int32_t aIndex) {
  if (aRoot.IsProxy()) {
    int32_t offset = 0;
    uint64_t containerID = 0;
    DocAccessibleParent* ipcDoc = aRoot.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendOffsetAtIndex(
        aRoot.AsProxy()->ID(), aIndex, &containerID, &offset);
    RemoteAccessible* container = ipcDoc->GetAccessible(containerID);
    return GeckoTextMarker(container, offset);
  } else if (auto htWrap = static_cast<HyperTextAccessibleWrap*>(
                 aRoot.AsAccessible()->AsHyperText())) {
    int32_t offset = 0;
    HyperTextAccessible* container = nullptr;
    htWrap->OffsetAtIndex(aIndex, &container, &offset);
    return GeckoTextMarker(container, offset);
  }

  return GeckoTextMarker();
}

id GeckoTextMarker::CreateAXTextMarker() {
  if (!IsValid()) {
    return nil;
  }

  AccessibleOrProxy doc;
  if (mContainer.IsProxy()) {
    doc = mContainer.AsProxy()->Document();
  } else {
    doc = mContainer.AsAccessible()->Document();
  }

  uintptr_t identifier =
      mContainer.IsProxy()
          ? mContainer.AsProxy()->ID()
          : reinterpret_cast<uintptr_t>(mContainer.AsAccessible()->UniqueID());

  OpaqueGeckoTextMarker opaqueMarker(doc.Bits(), identifier, mOffset);
  AXTextMarkerRef cf_text_marker = AXTextMarkerCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(&opaqueMarker),
      sizeof(OpaqueGeckoTextMarker));

  return [static_cast<id>(cf_text_marker) autorelease];
}

bool GeckoTextMarker::operator<(const GeckoTextMarker& aPoint) const {
  if (mContainer == aPoint.mContainer) return mOffset < aPoint.mOffset;

  // Build the chain of parents
  AutoTArray<AccessibleOrProxy, 30> parents1, parents2;
  AccessibleOrProxy p1 = mContainer;
  while (!p1.IsNull()) {
    parents1.AppendElement(p1);
    p1 = p1.Parent();
  }

  AccessibleOrProxy p2 = aPoint.mContainer;
  while (!p2.IsNull()) {
    parents2.AppendElement(p2);
    p2 = p2.Parent();
  }

  // An empty chain of parents means one of the containers was null.
  MOZ_ASSERT(parents1.Length() != 0 && parents2.Length() != 0,
             "have empty chain of parents!");

  // Find where the parent chain differs
  uint32_t pos1 = parents1.Length(), pos2 = parents2.Length();
  for (uint32_t len = std::min(pos1, pos2); len > 0; --len) {
    AccessibleOrProxy child1 = parents1.ElementAt(--pos1);
    AccessibleOrProxy child2 = parents2.ElementAt(--pos2);
    if (child1 != child2) {
      return child1.IndexInParent() < child2.IndexInParent();
    }
  }

  if (pos1 != 0) {
    // If parents1 is a superset of parents2 then mContainer is a
    // descendant of aPoint.mContainer. The next element down in parents1
    // is mContainer's ancestor that is the child of aPoint.mContainer.
    // We compare its end offset in aPoint.mContainer with aPoint.mOffset.
    AccessibleOrProxy child = parents1.ElementAt(pos1 - 1);
    MOZ_ASSERT(child.Parent() == aPoint.mContainer);
    bool unused;
    uint32_t endOffset = child.IsProxy() ? child.AsProxy()->EndOffset(&unused)
                                         : child.AsAccessible()->EndOffset();
    return endOffset < static_cast<uint32_t>(aPoint.mOffset);
  }

  if (pos2 != 0) {
    // If parents2 is a superset of parents1 then aPoint.mContainer is a
    // descendant of mContainer. The next element down in parents2
    // is aPoint.mContainer's ancestor that is the child of mContainer.
    // We compare its start offset in mContainer with mOffset.
    AccessibleOrProxy child = parents2.ElementAt(pos2 - 1);
    MOZ_ASSERT(child.Parent() == mContainer);
    bool unused;
    uint32_t startOffset = child.IsProxy()
                               ? child.AsProxy()->StartOffset(&unused)
                               : child.AsAccessible()->StartOffset();
    return static_cast<uint32_t>(mOffset) <= startOffset;
  }

  MOZ_ASSERT_UNREACHABLE("Broken tree?!");
  return false;
}

bool GeckoTextMarker::IsEditableRoot() {
  uint64_t state = mContainer.IsProxy() ? mContainer.AsProxy()->State()
                                        : mContainer.AsAccessible()->State();
  if ((state & states::EDITABLE) == 0) {
    return false;
  }

  AccessibleOrProxy parent = mContainer.Parent();
  if (parent.IsNull()) {
    // Not sure when this can happen, but it would technically be an editable
    // root.
    return true;
  }

  state = parent.IsProxy() ? parent.AsProxy()->State()
                           : parent.AsAccessible()->State();

  return (state & states::EDITABLE) == 0;
}

bool GeckoTextMarker::Next() {
  if (mContainer.IsProxy()) {
    int32_t nextOffset = 0;
    uint64_t nextContainerID = 0;
    DocAccessibleParent* ipcDoc = mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendNextClusterAt(
        mContainer.AsProxy()->ID(), mOffset, &nextContainerID, &nextOffset);
    RemoteAccessible* nextContainer = ipcDoc->GetAccessible(nextContainerID);
    bool moved = nextContainer != mContainer.AsProxy() || nextOffset != mOffset;
    mContainer = nextContainer;
    mOffset = nextOffset;
    return moved;
  } else if (auto htWrap = ContainerAsHyperTextWrap()) {
    HyperTextAccessible* nextContainer = nullptr;
    int32_t nextOffset = 0;
    htWrap->NextClusterAt(mOffset, &nextContainer, &nextOffset);
    bool moved = nextContainer != htWrap || nextOffset != mOffset;
    mContainer = nextContainer;
    mOffset = nextOffset;
    return moved;
  }

  return false;
}

bool GeckoTextMarker::Previous() {
  if (mContainer.IsProxy()) {
    int32_t prevOffset = 0;
    uint64_t prevContainerID = 0;
    DocAccessibleParent* ipcDoc = mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendPreviousClusterAt(
        mContainer.AsProxy()->ID(), mOffset, &prevContainerID, &prevOffset);
    RemoteAccessible* prevContainer = ipcDoc->GetAccessible(prevContainerID);
    bool moved = prevContainer != mContainer.AsProxy() || prevOffset != mOffset;
    mContainer = prevContainer;
    mOffset = prevOffset;
    return moved;
  } else if (auto htWrap = ContainerAsHyperTextWrap()) {
    HyperTextAccessible* prevContainer = nullptr;
    int32_t prevOffset = 0;
    htWrap->PreviousClusterAt(mOffset, &prevContainer, &prevOffset);
    bool moved = prevContainer != htWrap || prevOffset != mOffset;
    mContainer = prevContainer;
    mOffset = prevOffset;
    return moved;
  }

  return false;
}

static uint32_t CharacterCount(const AccessibleOrProxy& aContainer) {
  if (aContainer.IsProxy()) {
    return aContainer.AsProxy()->CharacterCount();
  }

  if (aContainer.AsAccessible()->IsHyperText()) {
    return aContainer.AsAccessible()->AsHyperText()->CharacterCount();
  }

  return 0;
}

GeckoTextMarkerRange GeckoTextMarker::Range(EWhichRange aRangeType) {
  MOZ_ASSERT(!mContainer.IsNull());
  if (mContainer.IsProxy()) {
    int32_t startOffset = 0, endOffset = 0;
    uint64_t startContainerID = 0, endContainerID = 0;
    DocAccessibleParent* ipcDoc = mContainer.AsProxy()->Document();
    bool success = ipcDoc->GetPlatformExtension()->SendRangeAt(
        mContainer.AsProxy()->ID(), mOffset, aRangeType, &startContainerID,
        &startOffset, &endContainerID, &endOffset);
    if (success) {
      return GeckoTextMarkerRange(
          GeckoTextMarker(ipcDoc->GetAccessible(startContainerID), startOffset),
          GeckoTextMarker(ipcDoc->GetAccessible(endContainerID), endOffset));
    }
  } else if (auto htWrap = ContainerAsHyperTextWrap()) {
    int32_t startOffset = 0, endOffset = 0;
    HyperTextAccessible* startContainer = nullptr;
    HyperTextAccessible* endContainer = nullptr;
    htWrap->RangeAt(mOffset, aRangeType, &startContainer, &startOffset,
                    &endContainer, &endOffset);
    return GeckoTextMarkerRange(GeckoTextMarker(startContainer, startOffset),
                                GeckoTextMarker(endContainer, endOffset));
  }

  return GeckoTextMarkerRange(GeckoTextMarker(), GeckoTextMarker());
}

AccessibleOrProxy GeckoTextMarker::Leaf() {
  MOZ_ASSERT(!mContainer.IsNull());
  if (mContainer.IsProxy()) {
    uint64_t leafID = 0;
    DocAccessibleParent* ipcDoc = mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendLeafAtOffset(
        mContainer.AsProxy()->ID(), mOffset, &leafID);
    return ipcDoc->GetAccessible(leafID);
  } else if (auto htWrap = ContainerAsHyperTextWrap()) {
    return htWrap->LeafAtOffset(mOffset);
  }

  return mContainer;
}

// GeckoTextMarkerRange

GeckoTextMarkerRange::GeckoTextMarkerRange(
    AccessibleOrProxy aDoc, AXTextMarkerRangeRef aTextMarkerRange) {
  if (!aTextMarkerRange ||
      CFGetTypeID(aTextMarkerRange) != AXTextMarkerRangeGetTypeID()) {
    return;
  }

  AXTextMarkerRef start_marker(
      AXTextMarkerRangeCopyStartMarker(aTextMarkerRange));
  AXTextMarkerRef end_marker(AXTextMarkerRangeCopyEndMarker(aTextMarkerRange));

  mStart = GeckoTextMarker(aDoc, start_marker);
  mEnd = GeckoTextMarker(aDoc, end_marker);

  CFRelease(start_marker);
  CFRelease(end_marker);
}

GeckoTextMarkerRange::GeckoTextMarkerRange(
    const AccessibleOrProxy& aAccessible) {
  if ((aAccessible.IsAccessible() &&
       aAccessible.AsAccessible()->IsHyperText()) ||
      (aAccessible.IsProxy() && aAccessible.AsProxy()->IsHyperText())) {
    // The accessible is a hypertext. Initialize range to its inner text range.
    mStart = GeckoTextMarker(aAccessible, 0);
    mEnd = GeckoTextMarker(aAccessible, (CharacterCount(aAccessible)));
  } else {
    // The accessible is not a hypertext (maybe a text leaf?). Initialize range
    // to its offsets in its container.
    mStart = GeckoTextMarker(aAccessible.Parent(), 0);
    mEnd = GeckoTextMarker(aAccessible.Parent(), 0);
    if (mStart.mContainer.IsProxy()) {
      DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
      Unused << ipcDoc->GetPlatformExtension()->SendRangeOfChild(
          mStart.mContainer.AsProxy()->ID(), aAccessible.AsProxy()->ID(),
          &mStart.mOffset, &mEnd.mOffset);
    } else if (auto htWrap = mStart.ContainerAsHyperTextWrap()) {
      htWrap->RangeOfChild(aAccessible.AsAccessible(), &mStart.mOffset,
                           &mEnd.mOffset);
    }
  }
}

id GeckoTextMarkerRange::CreateAXTextMarkerRange() {
  if (!IsValid()) {
    return nil;
  }

  AXTextMarkerRangeRef cf_text_marker_range =
      AXTextMarkerRangeCreate(kCFAllocatorDefault, mStart.CreateAXTextMarker(),
                              mEnd.CreateAXTextMarker());
  return [static_cast<id>(cf_text_marker_range) autorelease];
}

NSString* GeckoTextMarkerRange::Text() const {
  nsAutoString text;
  if (mStart.mContainer.IsProxy() && mEnd.mContainer.IsProxy()) {
    DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendTextForRange(
        mStart.mContainer.AsProxy()->ID(), mStart.mOffset,
        mEnd.mContainer.AsProxy()->ID(), mEnd.mOffset, &text);
  } else if (auto htWrap = mStart.ContainerAsHyperTextWrap()) {
    htWrap->TextForRange(text, mStart.mOffset, mEnd.ContainerAsHyperTextWrap(),
                         mEnd.mOffset);
  }
  return nsCocoaUtils::ToNSString(text);
}

static NSColor* ColorFromString(const nsString& aColorStr) {
  uint32_t r, g, b;
  if (sscanf(NS_ConvertUTF16toUTF8(aColorStr).get(), "rgb(%u, %u, %u)", &r, &g,
             &b) > 0) {
    return [NSColor colorWithCalibratedRed:(CGFloat)r / 0xff
                                     green:(CGFloat)g / 0xff
                                      blue:(CGFloat)b / 0xff
                                     alpha:1.0];
  }

  return nil;
}

static NSDictionary* StringAttributesFromAttributes(
    nsTArray<Attribute>& aAttributes, const AccessibleOrProxy& aContainer) {
  NSMutableDictionary* attrDict =
      [NSMutableDictionary dictionaryWithCapacity:aAttributes.Length()];
  NSMutableDictionary* fontAttrDict = [[NSMutableDictionary alloc] init];
  [attrDict setObject:fontAttrDict forKey:@"AXFont"];
  for (size_t ii = 0; ii < aAttributes.Length(); ii++) {
    RefPtr<nsAtom> attrName = NS_Atomize(aAttributes.ElementAt(ii).Name());
    if (attrName == nsGkAtoms::backgroundColor) {
      if (NSColor* color = ColorFromString(aAttributes.ElementAt(ii).Value())) {
        [attrDict setObject:(__bridge id)color.CGColor
                     forKey:@"AXBackgroundColor"];
      }
    } else if (attrName == nsGkAtoms::color) {
      if (NSColor* color = ColorFromString(aAttributes.ElementAt(ii).Value())) {
        [attrDict setObject:(__bridge id)color.CGColor
                     forKey:@"AXForegroundColor"];
      }
    } else if (attrName == nsGkAtoms::font_size) {
      float fontPointSize = 0;
      if (sscanf(NS_ConvertUTF16toUTF8(aAttributes.ElementAt(ii).Value()).get(),
                 "%fpt", &fontPointSize) > 0) {
        int32_t fontPixelSize = static_cast<int32_t>(fontPointSize * 4 / 3);
        [fontAttrDict setObject:@(fontPixelSize) forKey:@"AXFontSize"];
      }
    } else if (attrName == nsGkAtoms::font_family) {
      [fontAttrDict
          setObject:nsCocoaUtils::ToNSString(aAttributes.ElementAt(ii).Value())
             forKey:@"AXFontFamily"];
    } else if (attrName == nsGkAtoms::textUnderlineColor) {
      [attrDict setObject:@1 forKey:@"AXUnderline"];
      if (NSColor* color = ColorFromString(aAttributes.ElementAt(ii).Value())) {
        [attrDict setObject:(__bridge id)color.CGColor
                     forKey:@"AXUnderlineColor"];
      }
    } else if (attrName == nsGkAtoms::invalid) {
      // XXX: There is currently no attribute for grammar
      if (aAttributes.ElementAt(ii).Value().EqualsLiteral("spelling")) {
        [attrDict setObject:@YES
                     forKey:NSAccessibilityMarkedMisspelledTextAttribute];
      }
    } else {
      [attrDict
          setObject:nsCocoaUtils::ToNSString(aAttributes.ElementAt(ii).Value())
             forKey:nsCocoaUtils::ToNSString(NS_ConvertUTF8toUTF16(
                        aAttributes.ElementAt(ii).Name()))];
    }
  }

  mozAccessible* container = GetNativeFromGeckoAccessible(aContainer);
  id<MOXAccessible> link =
      [container moxFindAncestor:^BOOL(id<MOXAccessible> moxAcc, BOOL* stop) {
        return [[moxAcc moxRole] isEqualToString:NSAccessibilityLinkRole];
      }];
  if (link) {
    [attrDict setObject:link forKey:@"AXLink"];
  }

  id<MOXAccessible> heading =
      [container moxFindAncestor:^BOOL(id<MOXAccessible> moxAcc, BOOL* stop) {
        return [[moxAcc moxRole] isEqualToString:@"AXHeading"];
      }];
  if (heading) {
    [attrDict setObject:[heading moxValue] forKey:@"AXHeadingLevel"];
  }

  return attrDict;
}

NSAttributedString* GeckoTextMarkerRange::AttributedText() const {
  NSMutableAttributedString* str =
      [[[NSMutableAttributedString alloc] init] autorelease];

  if (mStart.mContainer.IsProxy() && mEnd.mContainer.IsProxy()) {
    nsTArray<TextAttributesRun> textAttributesRuns;
    DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendAttributedTextForRange(
        mStart.mContainer.AsProxy()->ID(), mStart.mOffset,
        mEnd.mContainer.AsProxy()->ID(), mEnd.mOffset, &textAttributesRuns);

    for (size_t i = 0; i < textAttributesRuns.Length(); i++) {
      nsTArray<Attribute>& attributes =
          textAttributesRuns.ElementAt(i).TextAttributes();
      RemoteAccessible* container =
          ipcDoc->GetAccessible(textAttributesRuns.ElementAt(i).ContainerID());

      NSAttributedString* substr = [[[NSAttributedString alloc]
          initWithString:nsCocoaUtils::ToNSString(
                             textAttributesRuns.ElementAt(i).Text())
              attributes:StringAttributesFromAttributes(attributes, container)]
          autorelease];

      [str appendAttributedString:substr];
    }
  } else if (auto htWrap = mStart.ContainerAsHyperTextWrap()) {
    nsTArray<nsString> texts;
    nsTArray<LocalAccessible*> containers;
    nsTArray<nsCOMPtr<nsIPersistentProperties>> props;

    htWrap->AttributedTextForRange(texts, props, containers, mStart.mOffset,
                                   mEnd.ContainerAsHyperTextWrap(),
                                   mEnd.mOffset);

    MOZ_ASSERT(texts.Length() == props.Length() &&
               texts.Length() == containers.Length());

    for (size_t i = 0; i < texts.Length(); i++) {
      nsTArray<Attribute> attributes;
      nsAccUtils::PersistentPropertiesToArray(props.ElementAt(i), &attributes);

      NSAttributedString* substr = [[[NSAttributedString alloc]
          initWithString:nsCocoaUtils::ToNSString(texts.ElementAt(i))
              attributes:StringAttributesFromAttributes(
                             attributes, containers.ElementAt(i))] autorelease];
      [str appendAttributedString:substr];
    }
  }

  return str;
}

int32_t GeckoTextMarkerRange::Length() const {
  int32_t length = 0;
  if (mStart.mContainer.IsProxy() && mEnd.mContainer.IsProxy()) {
    DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendLengthForRange(
        mStart.mContainer.AsProxy()->ID(), mStart.mOffset,
        mEnd.mContainer.AsProxy()->ID(), mEnd.mOffset, &length);
  } else if (auto htWrap = mStart.ContainerAsHyperTextWrap()) {
    length = htWrap->LengthForRange(
        mStart.mOffset, mEnd.ContainerAsHyperTextWrap(), mEnd.mOffset);
  }

  return length;
}

NSValue* GeckoTextMarkerRange::Bounds() const {
  nsIntRect rect;
  if (mStart.mContainer.IsProxy() && mEnd.mContainer.IsProxy()) {
    DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendBoundsForRange(
        mStart.mContainer.AsProxy()->ID(), mStart.mOffset,
        mEnd.mContainer.AsProxy()->ID(), mEnd.mOffset, &rect);
  } else if (auto htWrap = mStart.ContainerAsHyperTextWrap()) {
    rect = htWrap->BoundsForRange(
        mStart.mOffset, mEnd.ContainerAsHyperTextWrap(), mEnd.mOffset);
  }

  NSScreen* mainView = [[NSScreen screens] objectAtIndex:0];
  CGFloat scaleFactor = nsCocoaUtils::GetBackingScaleFactor(mainView);
  NSRect r =
      NSMakeRect(static_cast<CGFloat>(rect.x) / scaleFactor,
                 [mainView frame].size.height -
                     static_cast<CGFloat>(rect.y + rect.height) / scaleFactor,
                 static_cast<CGFloat>(rect.width) / scaleFactor,
                 static_cast<CGFloat>(rect.height) / scaleFactor);

  return [NSValue valueWithRect:r];
}

void GeckoTextMarkerRange::Select() const {
  if (mStart.mContainer.IsProxy() && mEnd.mContainer.IsProxy()) {
    DocAccessibleParent* ipcDoc = mStart.mContainer.AsProxy()->Document();
    Unused << ipcDoc->GetPlatformExtension()->SendSelectRange(
        mStart.mContainer.AsProxy()->ID(), mStart.mOffset,
        mEnd.mContainer.AsProxy()->ID(), mEnd.mOffset);
  } else if (RefPtr<HyperTextAccessibleWrap> htWrap =
                 mStart.ContainerAsHyperTextWrap()) {
    RefPtr<HyperTextAccessibleWrap> end = mEnd.ContainerAsHyperTextWrap();
    htWrap->SelectRange(mStart.mOffset, end, mEnd.mOffset);
  }
}

bool GeckoTextMarkerRange::Crop(const AccessibleOrProxy& aContainer) {
  GeckoTextMarker containerStart(aContainer, 0);
  GeckoTextMarker containerEnd(aContainer, CharacterCount(aContainer));

  if (mEnd < containerStart || containerEnd < mStart) {
    // The range ends before the container, or starts after it.
    return false;
  }

  if (mStart < containerStart) {
    // If range start is before container start, adjust range start to
    // start of container.
    mStart = containerStart;
  }

  if (containerEnd < mEnd) {
    // If range end is after container end, adjust range end to end of
    // container.
    mEnd = containerEnd;
  }

  return true;
}
}
}
