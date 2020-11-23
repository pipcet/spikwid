/* -*- Mode: C++; tab-width: 40; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNativeBasicThemeWin.h"

#include "LookAndFeel.h"

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeCheckboxColors(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeCheckboxColors(aState);
  }

  bool isDisabled = aState.HasState(NS_EVENT_STATE_DISABLED);
  bool isChecked = aState.HasState(NS_EVENT_STATE_CHECKED);

  sRGBColor backgroundColor = sRGBColor::FromABGR(
      LookAndFeel::GetColor(LookAndFeel::ColorID::TextBackground));
  sRGBColor borderColor = sRGBColor::FromABGR(
      LookAndFeel::GetColor(LookAndFeel::ColorID::Buttontext));
  if (isDisabled && isChecked) {
    backgroundColor = borderColor = sRGBColor::FromABGR(
        LookAndFeel::GetColor(LookAndFeel::ColorID::Graytext));
  } else if (isDisabled) {
    borderColor = sRGBColor::FromABGR(
        LookAndFeel::GetColor(LookAndFeel::ColorID::Graytext));
  } else if (isChecked) {
    backgroundColor = borderColor = sRGBColor::FromABGR(
        LookAndFeel::GetColor(LookAndFeel::ColorID::Highlight));
  }

  return std::make_pair(backgroundColor, borderColor);
}

sRGBColor nsNativeBasicThemeWin::ComputeCheckmarkColor(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeCheckmarkColor(aState);
  }

  return sRGBColor::FromABGR(
      LookAndFeel::GetColor(LookAndFeel::ColorID::TextBackground));
}

std::pair<sRGBColor, sRGBColor>
nsNativeBasicThemeWin::ComputeRadioCheckmarkColors(const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeRadioCheckmarkColors(aState);
  }

  auto [unusedColor, checkColor] = ComputeCheckboxColors(aState);
  (void)unusedColor;
  sRGBColor backgroundColor = ComputeCheckmarkColor(aState);

  return std::make_pair(backgroundColor, checkColor);
}

sRGBColor nsNativeBasicThemeWin::ComputeBorderColor(const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeBorderColor(aState);
  }

  bool isDisabled = aState.HasState(NS_EVENT_STATE_DISABLED);

  if (isDisabled) {
    return sRGBColor::FromABGR(
        LookAndFeel::GetColor(LookAndFeel::ColorID::Graytext));
  }
  return sRGBColor::FromABGR(
      LookAndFeel::GetColor(LookAndFeel::ColorID::Buttontext));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeButtonColors(
    const EventStates& aState, nsIFrame* aFrame) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeButtonColors(aState, aFrame);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Buttonface)),
                        ComputeBorderColor(aState));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeTextfieldColors(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeTextfieldColors(aState);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextBackground)),
                        ComputeBorderColor(aState));
}

std::pair<sRGBColor, sRGBColor>
nsNativeBasicThemeWin::ComputeRangeProgressColors(const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeRangeProgressColors(aState);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Highlight)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Buttontext)));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeRangeTrackColors(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeRangeTrackColors(aState);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextBackground)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Buttontext)));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeRangeThumbColors(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeRangeThumbColors(aState);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Highlight)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Highlight)));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeProgressColors() {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeProgressColors();
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Highlight)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Buttontext)));
}

std::pair<sRGBColor, sRGBColor>
nsNativeBasicThemeWin::ComputeProgressTrackColors() {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeProgressTrackColors();
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextBackground)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Buttontext)));
}

std::pair<sRGBColor, sRGBColor> nsNativeBasicThemeWin::ComputeMeterchunkColors(
    const double aValue, const double aOptimum, const double aLow) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeMeterchunkColors(aValue, aOptimum, aLow);
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::Highlight)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextForeground)));
}

std::pair<sRGBColor, sRGBColor>
nsNativeBasicThemeWin::ComputeMeterTrackColors() {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeMeterTrackColors();
  }

  return std::make_pair(sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextBackground)),
                        sRGBColor::FromABGR(LookAndFeel::GetColor(
                            LookAndFeel::ColorID::TextForeground)));
}

sRGBColor nsNativeBasicThemeWin::ComputeMenulistArrowButtonColor(
    const EventStates& aState) {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeMenulistArrowButtonColor(aState);
  }

  bool isDisabled = aState.HasState(NS_EVENT_STATE_DISABLED);

  if (isDisabled) {
    return sRGBColor::FromABGR(
        LookAndFeel::GetColor(LookAndFeel::ColorID::Graytext));
  }
  return sRGBColor::FromABGR(
      LookAndFeel::GetColor(LookAndFeel::ColorID::TextForeground));
}

std::array<sRGBColor, 3> nsNativeBasicThemeWin::ComputeFocusRectColors() {
  if (!LookAndFeel::GetInt(LookAndFeel::IntID::UseAccessibilityTheme, 0)) {
    return nsNativeBasicTheme::ComputeFocusRectColors();
  }

  return {sRGBColor::FromABGR(
              LookAndFeel::GetColor(LookAndFeel::ColorID::Highlight)),
          sRGBColor::FromABGR(
              LookAndFeel::GetColor(LookAndFeel::ColorID::Buttontext)),
          sRGBColor::FromABGR(
              LookAndFeel::GetColor(LookAndFeel::ColorID::TextBackground))};
}

already_AddRefed<nsITheme> do_GetBasicNativeThemeDoNotUseDirectly() {
  static StaticRefPtr<nsITheme> gInstance;
  if (MOZ_UNLIKELY(!gInstance)) {
    gInstance = new nsNativeBasicThemeWin();
    ClearOnShutdown(&gInstance);
  }
  return do_AddRef(gInstance);
}
