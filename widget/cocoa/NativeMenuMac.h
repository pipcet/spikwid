/* -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NativeMenuMac_h
#define NativeMenuMac_h

#include "mozilla/widget/NativeMenu.h"

#include "nsMenuItemIconX.h"
#include "nsMenuX.h"

class nsIContent;
class nsMenuGroupOwnerX;

namespace mozilla {

namespace dom {
class Element;
}

namespace widget {

class NativeMenuMac : public NativeMenu,
                      public nsMenuItemIconX::Listener,
                      public nsMenuX::Observer {
 public:
  explicit NativeMenuMac(mozilla::dom::Element* aElement);

  // NativeMenu
  bool ShowAsContextMenu(const mozilla::DesktopPoint& aPosition) override;
  void AddObserver(NativeMenu::Observer* aObserver) override {
    mObservers.AppendElement(aObserver);
  }
  void RemoveObserver(NativeMenu::Observer* aObserver) override {
    mObservers.RemoveElement(aObserver);
  }

  // nsMenuItemIconX::Listener
  void IconUpdated() override;

  // nsMenuX::Observer
  void OnMenuOpened() override;
  void OnMenuClosed() override;

  NSMenu* NativeNSMenu() { return mMenu ? mMenu->NativeNSMenu() : nil; }
  void MenuWillOpen();

  // Returns whether a menu item was found at the specified path.
  bool ActivateNativeMenuItemAt(const nsAString& aIndexString);

  void ForceUpdateNativeMenuAt(const nsAString& aIndexString);
  void Dump();

  // If this menu is the menu of a system status bar item (NSStatusItem),
  // let the menu know about the status item so that it can propagate
  // any icon changes to the status item.
  void SetContainerStatusBarItem(NSStatusItem* aItem);

 protected:
  virtual ~NativeMenuMac();

  // Proceed with showing the menu after a call to ShowAsContextMenu.
  // This is done from a runnable so that the nested event loop doesn't run
  // during ShowAsContextMenu.
  void OpenMenu(const mozilla::DesktopPoint& aPosition);

  RefPtr<nsIContent> mContent;
  RefPtr<nsMenuGroupOwnerX> mMenuGroupOwner;
  RefPtr<nsMenuX> mMenu;
  nsTArray<NativeMenu::Observer*> mObservers;
  NSStatusItem* mContainerStatusBarItem;
};

}  // namespace widget
}  // namespace mozilla

#endif
