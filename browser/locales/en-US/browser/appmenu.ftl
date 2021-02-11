# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

## App Menu

appmenuitem-update-banner =
    .label-update-downloading = Downloading { -brand-shorter-name } update
appmenuitem-protection-dashboard-title = "Protections" Dashboard
appmenuitem-customize-mode =
    .label = Customize…
appmenuitem-new-window =
    .label = New Window
appmenuitem-new-private-window =
    .label = New Private Window
appmenuitem-passwords =
    .label = Passwords
appmenuitem-extensions-and-themes =
    .label = Extensions and Themes
appmenuitem-find-in-page =
    .label = Find In Page…
appmenuitem-more-tools =
    .label = More Tools
appmenuitem-exit =
    .label = Exit

# Settings is now used to access the browser settings across all platforms,
# instead of Options or Preferences.
appmenuitem-settings =
    .label = Settings

## Zoom and Fullscreen Controls

appmenuitem-zoom-enlarge =
  .label = Zoom in
appmenuitem-zoom-reduce =
  .label = Zoom out
appmenuitem-fullscreen =
  .label = Full Screen

## Firefox Account toolbar button and Sync panel in App menu.

fxa-toolbar-sync-now =
    .label = Sync Now

appmenuitem-save-page =
    .label = Save Page As…

## What's New panel in App menu.

whatsnew-panel-header = What’s New

# Checkbox displayed at the bottom of the What's New panel, allowing users to
# enable/disable What's New notifications.
whatsnew-panel-footer-checkbox =
  .label = Notify about new features
  .accesskey = f

## The Firefox Profiler – The popup is the UI to turn on the profiler, and record
## performance profiles. To enable it go to profiler.firefox.com and click
## "Enable Profiler Menu Button".

profiler-popup-title =
  .value = { -profiler-brand-name }

profiler-popup-reveal-description-button =
  .aria-label = Reveal more information

profiler-popup-description-title =
  .value = Record, analyze, share

profiler-popup-description =
  Collaborate on performance issues by publishing profiles to share with your team.

profiler-popup-learn-more = Learn more

profiler-popup-settings =
  .value = Settings

# This link takes the user to about:profiling, and is only visible with the Custom preset.
profiler-popup-edit-settings = Edit Settings…

profiler-popup-disabled =
  The profiler is currently disabled, most likely due to a Private Browsing window
  being open.

profiler-popup-recording-screen = Recording…

# The profiler presets list is generated elsewhere, but the custom preset is defined
# here only.
profiler-popup-presets-custom =
  .label = Custom

profiler-popup-start-recording-button =
  .label = Start Recording

profiler-popup-discard-button =
  .label = Discard

profiler-popup-capture-button =
  .label = Capture

profiler-popup-start-shortcut =
  { PLATFORM() ->
      [macos] ⌃⇧1
     *[other] Ctrl+Shift+1
  }

profiler-popup-capture-shortcut =
  { PLATFORM() ->
      [macos] ⌃⇧2
     *[other] Ctrl+Shift+2
  }

## History panel

appmenu-manage-history =
    .label = Manage History
appmenu-reopen-all-tabs = Reopen All Tabs
appmenu-reopen-all-windows = Reopen All Windows

## Help panel
appmenu-help-header =
    .title = { -brand-shorter-name } Help
appmenu-about =
    .label = About { -brand-shorter-name }
    .accesskey = A
appmenu-get-help =
    .label = Get Help
    .accesskey = H
appmenu-help-troubleshooting-info =
    .label = Troubleshooting Information
    .accesskey = T
appmenu-help-taskmanager =
    .label = Task Manager
appmenu-help-feedback-page =
    .label = Submit Feedback…
    .accesskey = S

## appmenu-help-report-deceptive-site and appmenu-help-not-deceptive
## are mutually exclusive, so it's possible to use the same accesskey for both.

appmenu-help-report-deceptive-site =
    .label = Report Deceptive Site…
    .accesskey = D
appmenu-help-not-deceptive =
    .label = This Isn’t a Deceptive Site…
    .accesskey = D

## More Tools

appmenu-customizetoolbar =
    .label = Customize Toolbar…

appmenu-developer-tools-subheader = Browser Tools
