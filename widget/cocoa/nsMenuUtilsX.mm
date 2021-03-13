/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsMenuUtilsX.h"

#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/XULCommandEvent.h"
#include "nsMenuBarX.h"
#include "nsMenuX.h"
#include "nsMenuItemX.h"
#include "nsStandaloneNativeMenu.h"
#include "nsObjCExceptions.h"
#include "nsCocoaUtils.h"
#include "nsCocoaWindow.h"
#include "nsGkAtoms.h"
#include "nsGlobalWindowInner.h"
#include "nsPIDOMWindow.h"
#include "nsQueryObject.h"

using namespace mozilla;

void nsMenuUtilsX::DispatchCommandTo(nsIContent* aTargetContent) {
  MOZ_ASSERT(aTargetContent, "null ptr");

  dom::Document* doc = aTargetContent->OwnerDoc();
  if (doc) {
    RefPtr<dom::XULCommandEvent> event =
        new dom::XULCommandEvent(doc, doc->GetPresContext(), nullptr);

    IgnoredErrorResult rv;
    event->InitCommandEvent(u"command"_ns, true, true,
                            nsGlobalWindowInner::Cast(doc->GetInnerWindow()), 0, false, false,
                            false, false, nullptr, 0, rv);
    // FIXME: Should probably figure out how to init this with the actual
    // pressed keys, but this is a big old edge case anyway. -dwh
    if (!rv.Failed()) {
      event->SetTrusted(true);
      aTargetContent->DispatchEvent(*event);
    }
  }
}

NSString* nsMenuUtilsX::GetTruncatedCocoaLabel(const nsString& itemLabel) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  // We want to truncate long strings to some reasonable pixel length but there is no
  // good API for doing that which works for all OS versions and architectures. For now
  // we'll do nothing for consistency and depend on good user interface design to limit
  // string lengths.
  return [NSString stringWithCharacters:reinterpret_cast<const unichar*>(itemLabel.get())
                                 length:itemLabel.Length()];

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

uint8_t nsMenuUtilsX::GeckoModifiersForNodeAttribute(const nsString& modifiersAttribute) {
  uint8_t modifiers = knsMenuItemNoModifier;
  char* str = ToNewCString(modifiersAttribute);
  char* newStr;
  char* token = strtok_r(str, ", \t", &newStr);
  while (token != nullptr) {
    if (strcmp(token, "shift") == 0) {
      modifiers |= knsMenuItemShiftModifier;
    } else if (strcmp(token, "alt") == 0) {
      modifiers |= knsMenuItemAltModifier;
    } else if (strcmp(token, "control") == 0) {
      modifiers |= knsMenuItemControlModifier;
    } else if ((strcmp(token, "accel") == 0) || (strcmp(token, "meta") == 0)) {
      modifiers |= knsMenuItemCommandModifier;
    }
    token = strtok_r(newStr, ", \t", &newStr);
  }
  free(str);

  return modifiers;
}

unsigned int nsMenuUtilsX::MacModifiersForGeckoModifiers(uint8_t geckoModifiers) {
  unsigned int macModifiers = 0;

  if (geckoModifiers & knsMenuItemShiftModifier) {
    macModifiers |= NSEventModifierFlagShift;
  }
  if (geckoModifiers & knsMenuItemAltModifier) {
    macModifiers |= NSEventModifierFlagOption;
  }
  if (geckoModifiers & knsMenuItemControlModifier) {
    macModifiers |= NSEventModifierFlagControl;
  }
  if (geckoModifiers & knsMenuItemCommandModifier) {
    macModifiers |= NSEventModifierFlagCommand;
  }

  return macModifiers;
}

nsMenuBarX* nsMenuUtilsX::GetHiddenWindowMenuBar() {
  nsIWidget* hiddenWindowWidgetNoCOMPtr = nsCocoaUtils::GetHiddenWindowWidget();
  if (hiddenWindowWidgetNoCOMPtr) {
    return static_cast<nsCocoaWindow*>(hiddenWindowWidgetNoCOMPtr)->GetMenuBar();
  }
  return nullptr;
}

// It would be nice if we could localize these edit menu names.
NSMenuItem* nsMenuUtilsX::GetStandardEditMenuItem() {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  // In principle we should be able to allocate this once and then always
  // return the same object.  But weird interactions happen between native
  // app-modal dialogs and Gecko-modal dialogs that open above them.  So what
  // we return here isn't always released before it needs to be added to
  // another menu.  See bmo bug 468393.
  NSMenuItem* standardEditMenuItem = [[[NSMenuItem alloc] initWithTitle:@"Edit"
                                                                 action:nil
                                                          keyEquivalent:@""] autorelease];
  NSMenu* standardEditMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  standardEditMenuItem.submenu = standardEditMenu;
  [standardEditMenu release];

  // Add Undo
  NSMenuItem* undoItem = [[NSMenuItem alloc] initWithTitle:@"Undo"
                                                    action:@selector(undo:)
                                             keyEquivalent:@"z"];
  [standardEditMenu addItem:undoItem];
  [undoItem release];

  // Add Redo
  NSMenuItem* redoItem = [[NSMenuItem alloc] initWithTitle:@"Redo"
                                                    action:@selector(redo:)
                                             keyEquivalent:@"Z"];
  [standardEditMenu addItem:redoItem];
  [redoItem release];

  // Add separator
  [standardEditMenu addItem:[NSMenuItem separatorItem]];

  // Add Cut
  NSMenuItem* cutItem = [[NSMenuItem alloc] initWithTitle:@"Cut"
                                                   action:@selector(cut:)
                                            keyEquivalent:@"x"];
  [standardEditMenu addItem:cutItem];
  [cutItem release];

  // Add Copy
  NSMenuItem* copyItem = [[NSMenuItem alloc] initWithTitle:@"Copy"
                                                    action:@selector(copy:)
                                             keyEquivalent:@"c"];
  [standardEditMenu addItem:copyItem];
  [copyItem release];

  // Add Paste
  NSMenuItem* pasteItem = [[NSMenuItem alloc] initWithTitle:@"Paste"
                                                     action:@selector(paste:)
                                              keyEquivalent:@"v"];
  [standardEditMenu addItem:pasteItem];
  [pasteItem release];

  // Add Delete
  NSMenuItem* deleteItem = [[NSMenuItem alloc] initWithTitle:@"Delete"
                                                      action:@selector(delete:)
                                               keyEquivalent:@""];
  [standardEditMenu addItem:deleteItem];
  [deleteItem release];

  // Add Select All
  NSMenuItem* selectAllItem = [[NSMenuItem alloc] initWithTitle:@"Select All"
                                                         action:@selector(selectAll:)
                                                  keyEquivalent:@"a"];
  [standardEditMenu addItem:selectAllItem];
  [selectAllItem release];

  return standardEditMenuItem;

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

bool nsMenuUtilsX::NodeIsHiddenOrCollapsed(nsIContent* aContent) {
  return aContent->IsElement() &&
         (aContent->AsElement()->AttrValueIs(kNameSpaceID_None, nsGkAtoms::hidden, nsGkAtoms::_true,
                                             eCaseMatters) ||
          aContent->AsElement()->AttrValueIs(kNameSpaceID_None, nsGkAtoms::collapsed,
                                             nsGkAtoms::_true, eCaseMatters));
}

// Determines how many items are visible among the siblings in a menu that are
// before the given child. This will not count the application menu.
int nsMenuUtilsX::CalculateNativeInsertionPoint(nsMenuObjectX* aParent, nsMenuObjectX* aChild) {
  int insertionPoint = 0;
  nsMenuObjectTypeX parentType = aParent->MenuObjectType();
  if (parentType == eMenuBarObjectType) {
    nsMenuBarX* menubarParent = static_cast<nsMenuBarX*>(aParent);
    uint32_t numMenus = menubarParent->GetMenuCount();
    for (uint32_t i = 0; i < numMenus; i++) {
      nsMenuX* currMenu = menubarParent->GetMenuAt(i);
      if (currMenu == aChild) {
        return insertionPoint;  // we found ourselves, break out
      }
      if (currMenu && currMenu->NativeMenuItem().menu) {
        insertionPoint++;
      }
    }
  } else if (parentType == eSubmenuObjectType || parentType == eStandaloneNativeMenuObjectType) {
    nsMenuX* menuParent;
    if (parentType == eSubmenuObjectType) {
      menuParent = static_cast<nsMenuX*>(aParent);
    } else {
      menuParent = static_cast<nsStandaloneNativeMenu*>(aParent)->GetMenuXObject();
    }

    uint32_t numItems = menuParent->GetItemCount();
    for (uint32_t i = 0; i < numItems; i++) {
      // Using GetItemAt instead of GetVisibleItemAt to avoid O(N^2)
      nsMenuObjectX* currItem = menuParent->GetItemAt(i);
      if (currItem == aChild) {
        return insertionPoint;  // we found ourselves, break out
      }
      NSMenuItem* nativeItem = nil;
      nsMenuObjectTypeX currItemType = currItem->MenuObjectType();
      if (currItemType == eSubmenuObjectType) {
        nativeItem = static_cast<nsMenuX*>(currItem)->NativeMenuItem();
      } else {
        nativeItem = (NSMenuItem*)(currItem->NativeData());
      }
      if (nativeItem.menu) {
        insertionPoint++;
      }
    }
  }
  return insertionPoint;
}

NSMenuItem* nsMenuUtilsX::NativeMenuItemWithLocation(NSMenu* aRootMenu, NSString* aLocationString,
                                                     bool aIsMenuBar) {
  NSArray<NSString*>* indexes = [aLocationString componentsSeparatedByString:@"|"];
  unsigned int pathLength = indexes.count;
  if (pathLength == 0) {
    return nil;
  }

  NSMenu* currentSubmenu = aRootMenu;
  for (unsigned int depth = 0; depth < pathLength; depth++) {
    NSInteger targetIndex = [indexes objectAtIndex:depth].integerValue;
    if (aIsMenuBar && depth == 0) {
      // We remove the application menu from consideration for the top-level menu.
      targetIndex++;
    }
    int itemCount = currentSubmenu.numberOfItems;
    if (targetIndex < itemCount) {
      NSMenuItem* menuItem = [currentSubmenu itemAtIndex:targetIndex];
      // if this is the last index just return the menu item
      if (depth == pathLength - 1) {
        return menuItem;
      }
      // if this is not the last index find the submenu and keep going
      if (menuItem.hasSubmenu) {
        currentSubmenu = menuItem.submenu;
      } else {
        return nil;
      }
    }
  }

  return nil;
}
