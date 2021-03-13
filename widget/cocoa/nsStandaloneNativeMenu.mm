/* -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#import <Cocoa/Cocoa.h>

#include "nsStandaloneNativeMenu.h"
#include "nsMenuItemX.h"
#include "nsMenuUtilsX.h"
#include "nsIMutationObserver.h"
#include "nsGkAtoms.h"
#include "nsObjCExceptions.h"
#include "mozilla/dom/Element.h"

using namespace mozilla;

using mozilla::dom::Element;

NS_IMPL_ISUPPORTS_INHERITED(nsStandaloneNativeMenu, nsMenuGroupOwnerX, nsIMutationObserver,
                            nsIStandaloneNativeMenu)

nsStandaloneNativeMenu::nsStandaloneNativeMenu() : mMenu(nullptr), mContainerStatusBarItem(nil) {}

nsStandaloneNativeMenu::~nsStandaloneNativeMenu() {}

NS_IMETHODIMP
nsStandaloneNativeMenu::Init(Element* aElement) {
  NS_ASSERTION(mMenu == nullptr, "nsNativeMenu::Init - mMenu not null!");

  NS_ENSURE_ARG(aElement);

  if (!aElement->IsAnyOfXULElements(nsGkAtoms::menu, nsGkAtoms::menupopup)) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv = nsMenuGroupOwnerX::Create(aElement);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mMenu = MakeUnique<nsMenuX>(this, this, aElement);
  mMenu->SetupIcon();

  return NS_OK;
}

static void UpdateMenu(nsMenuX* aMenu) {
  aMenu->MenuOpened();
  aMenu->MenuClosed();

  uint32_t itemCount = aMenu->GetItemCount();
  for (uint32_t i = 0; i < itemCount; i++) {
    nsMenuObjectX* menuObject = aMenu->GetItemAt(i);
    if (menuObject->MenuObjectType() == eSubmenuObjectType) {
      UpdateMenu(static_cast<nsMenuX*>(menuObject));
    }
  }
}

NS_IMETHODIMP
nsStandaloneNativeMenu::MenuWillOpen(bool* aResult) {
  NS_ASSERTION(mMenu != nullptr, "nsStandaloneNativeMenu::OnOpen - mMenu is null!");

  // Force an update on the mMenu by faking an open/close on all of
  // its submenus.
  UpdateMenu(mMenu.get());

  *aResult = true;
  return NS_OK;
}

NS_IMETHODIMP
nsStandaloneNativeMenu::GetNativeMenu(void** aVoidPointer) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  if (mMenu) {
    *aVoidPointer = mMenu->NativeData();
    [[(NSObject*)(*aVoidPointer) retain] autorelease];
    return NS_OK;
  }
  *aVoidPointer = nullptr;
  return NS_ERROR_NOT_INITIALIZED;

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

NS_IMETHODIMP
nsStandaloneNativeMenu::ActivateNativeMenuItemAt(const nsAString& indexString) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  if (!mMenu) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  NSString* locationString =
      [NSString stringWithCharacters:reinterpret_cast<const unichar*>(indexString.BeginReading())
                              length:indexString.Length()];
  NSMenu* menu = static_cast<NSMenu*>(mMenu->NativeData());
  NSMenuItem* item = nsMenuUtilsX::NativeMenuItemWithLocation(menu, locationString, false);

  // We can't perform an action on an item with a submenu, that will raise
  // an obj-c exception.
  if (item && !item.hasSubmenu) {
    NSMenu* parent = item.menu;
    if (parent) {
      // NSLog(@"Performing action for native menu item titled: %@\n",
      //       [[currentSubmenu itemAtIndex:targetIndex] title]);
      [parent performActionForItemAtIndex:[parent indexOfItem:item]];
      return NS_OK;
    }
  }

  return NS_ERROR_FAILURE;

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

NS_IMETHODIMP
nsStandaloneNativeMenu::ForceUpdateNativeMenuAt(const nsAString& indexString) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  if (!mMenu) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  NSString* locationString =
      [NSString stringWithCharacters:reinterpret_cast<const unichar*>(indexString.BeginReading())
                              length:indexString.Length()];
  NSArray<NSString*>* indexes = [locationString componentsSeparatedByString:@"|"];
  unsigned int indexCount = indexes.count;
  if (indexCount == 0) {
    return NS_OK;
  }

  nsMenuX* currentMenu = mMenu.get();

  // now find the correct submenu
  for (unsigned int i = 1; currentMenu && i < indexCount; i++) {
    int targetIndex = [indexes objectAtIndex:i].intValue;
    int visible = 0;
    uint32_t length = currentMenu->GetItemCount();
    for (unsigned int j = 0; j < length; j++) {
      nsMenuObjectX* targetMenu = currentMenu->GetItemAt(j);
      if (!targetMenu) {
        return NS_OK;
      }
      MOZ_RELEASE_ASSERT(targetMenu->MenuObjectType() == eSubmenuObjectType ||
                         targetMenu->MenuObjectType() == eMenuItemObjectType);
      RefPtr<nsIContent> content = targetMenu->MenuObjectType() == eSubmenuObjectType
                                       ? static_cast<nsMenuX*>(targetMenu)->Content()
                                       : static_cast<nsMenuItemX*>(targetMenu)->Content();
      if (!nsMenuUtilsX::NodeIsHiddenOrCollapsed(content)) {
        visible++;
        if (targetMenu->MenuObjectType() == eSubmenuObjectType && visible == (targetIndex + 1)) {
          currentMenu = static_cast<nsMenuX*>(targetMenu);
          // fake open/close to cause lazy update to happen
          currentMenu->MenuOpened();
          currentMenu->MenuClosed();
          break;
        }
      }
    }
  }

  return NS_OK;

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

void nsStandaloneNativeMenu::IconUpdated() {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  if (mContainerStatusBarItem) {
    NSImage* menuImage = mMenu->NativeMenuItem().image;
    if (menuImage) {
      [menuImage setTemplate:YES];
    }
    mContainerStatusBarItem.image = menuImage;
  }

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

void nsStandaloneNativeMenu::SetContainerStatusBarItem(NSStatusItem* aItem) {
  mContainerStatusBarItem = aItem;
  IconUpdated();
}
