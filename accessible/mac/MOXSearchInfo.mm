/* clang-format off */
/* -*- Mode: Objective-C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* clang-format on */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#import "MOXSearchInfo.h"
#import "MOXWebAreaAccessible.h"
#import "RotorRules.h"

#include "nsCocoaUtils.h"
#include "DocAccessibleParent.h"

using namespace mozilla::a11y;

@interface MOXSearchInfo ()
- (NSMutableArray*)getMatchesForRule:(PivotRule&)rule;

- (AccessibleOrProxy)rootGeckoAccessible;

- (AccessibleOrProxy)startGeckoAccessible;
@end

@implementation MOXSearchInfo

- (id)initWithParameters:(NSDictionary*)params
                 andRoot:(MOXAccessibleBase*)root {
  if (id searchKeyParam = [params objectForKey:@"AXSearchKey"]) {
    mSearchKeys = [searchKeyParam isKindOfClass:[NSString class]]
                      ? @[ searchKeyParam ]
                      : searchKeyParam;
  }

  if (id startElemParam = [params objectForKey:@"AXStartElement"]) {
    mStartElem = startElemParam;
  } else {
    mStartElem = root;
  }

  mRoot = root;

  mResultLimit = [[params objectForKey:@"AXResultsLimit"] intValue];

  mSearchForward =
      [[params objectForKey:@"AXDirection"] isEqualToString:@"AXDirectionNext"];

  mImmediateDescendantsOnly =
      [[params objectForKey:@"AXImmediateDescendantsOnly"] boolValue];

  return [super init];
}

- (AccessibleOrProxy)rootGeckoAccessible {
  id root =
      [mRoot isKindOfClass:[mozAccessible class]] ? mRoot : [mRoot moxParent];

  MOZ_ASSERT([mRoot isKindOfClass:[mozAccessible class]]);

  return [static_cast<mozAccessible*>(root) geckoAccessible];
}

- (AccessibleOrProxy)startGeckoAccessible {
  if ([mStartElem isKindOfClass:[mozAccessible class]]) {
    return [static_cast<mozAccessible*>(mStartElem) geckoAccessible];
  }

  // If it isn't a mozAccessible, it doesn't have a gecko accessible
  // this is most likely the root group. Use the gecko doc as the start
  // accessible.
  return [self rootGeckoAccessible];
}

- (NSMutableArray*)getMatchesForRule:(PivotRule&)rule {
  int resultLimit = mResultLimit;
  NSMutableArray* matches = [[NSMutableArray alloc] init];
  AccessibleOrProxy geckoRootAcc = [self rootGeckoAccessible];
  AccessibleOrProxy geckoStartAcc = [self startGeckoAccessible];
  Pivot p = Pivot(geckoRootAcc);
  AccessibleOrProxy match = mSearchForward ? p.Next(geckoStartAcc, rule)
                                           : p.Prev(geckoStartAcc, rule);
  while (!match.IsNull() && resultLimit != 0) {
    // we use mResultLimit != 0 to capture the case where mResultLimit is -1
    // when it is set from the params dictionary. If that's true, we want
    // to return all matches (ie. have no limit)
    mozAccessible* nativeMatch = GetNativeFromGeckoAccessible(match);
    if (nativeMatch) {
      // only add/count results for which there is a matching
      // native accessible
      [matches addObject:nativeMatch];
      resultLimit -= 1;
    }

    match = mSearchForward ? p.Next(match, rule) : p.Prev(match, rule);
  }

  return matches;
}

- (NSArray*)performSearch {
  AccessibleOrProxy geckoStartAcc = [self startGeckoAccessible];
  NSMutableArray* matches = [[NSMutableArray alloc] init];
  for (id key in mSearchKeys) {
    if ([key isEqualToString:@"AXAnyTypeSearchKey"]) {
      RotorRule rule =
          mImmediateDescendantsOnly ? RotorRule(geckoStartAcc) : RotorRule();

      if (mSearchForward) {
        if ([mStartElem isKindOfClass:[MOXWebAreaAccessible class]]) {
          if (id rootGroup =
                  [static_cast<MOXWebAreaAccessible*>(mStartElem) rootGroup]) {
            // Moving forward from web area, rootgroup; if it exists, is next.
            [matches addObject:rootGroup];
            if (mResultLimit == 1) {
              // Found one match, continue in search keys for block.
              continue;
            }
          }
        } else if (mImmediateDescendantsOnly &&
                   [mStartElem isKindOfClass:[MOXRootGroup class]]) {
          // Moving forward from root group. If we don't match descendants,
          // there is no match. Continue.
          continue;
        }
      } else if (!mSearchForward &&
                 [mStartElem isKindOfClass:[MOXRootGroup class]]) {
        // Moving backward from root group. Web area is next.
        [matches addObject:[mStartElem moxParent]];
        if (mResultLimit == 1) {
          // Found one match, continue in search keys for block.
          continue;
        }
      }
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXHeadingSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::HEADING, geckoStartAcc)
                               : RotorRoleRule(roles::HEADING);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXArticleSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::ARTICLE, geckoStartAcc)
                               : RotorRoleRule(roles::ARTICLE);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXTableSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::TABLE, geckoStartAcc)
                               : RotorRoleRule(roles::TABLE);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXLandmarkSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::LANDMARK, geckoStartAcc)
                               : RotorRoleRule(roles::LANDMARK);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXListSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::LIST, geckoStartAcc)
                               : RotorRoleRule(roles::LIST);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXLinkSearchKey"]) {
      RotorLinkRule rule = mImmediateDescendantsOnly
                               ? RotorLinkRule(geckoStartAcc)
                               : RotorLinkRule();
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXVisitedLinkSearchKey"]) {
      RotorVisitedLinkRule rule = mImmediateDescendantsOnly
                                      ? RotorVisitedLinkRule(geckoStartAcc)
                                      : RotorVisitedLinkRule();
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXUnvisitedLinkSearchKey"]) {
      RotorUnvisitedLinkRule rule = mImmediateDescendantsOnly
                                        ? RotorUnvisitedLinkRule(geckoStartAcc)
                                        : RotorUnvisitedLinkRule();
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXButtonSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::PUSHBUTTON, geckoStartAcc)
                               : RotorRoleRule(roles::PUSHBUTTON);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXControlSearchKey"]) {
      RotorControlRule rule = mImmediateDescendantsOnly
                                  ? RotorControlRule(geckoStartAcc)
                                  : RotorControlRule();
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXRadioGroupSearchKey"]) {
      RotorRoleRule rule =
          mImmediateDescendantsOnly
              ? RotorRoleRule(roles::RADIO_GROUP, geckoStartAcc)
              : RotorRoleRule(roles::RADIO_GROUP);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXFrameSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::DOCUMENT, geckoStartAcc)
                               : RotorRoleRule(roles::DOCUMENT);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXImageSearchKey"]) {
      RotorRoleRule rule = mImmediateDescendantsOnly
                               ? RotorRoleRule(roles::GRAPHIC, geckoStartAcc)
                               : RotorRoleRule(roles::GRAPHIC);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXCheckboxSearchKey"]) {
      RotorRoleRule rule =
          mImmediateDescendantsOnly
              ? RotorRoleRule(roles::CHECKBUTTON, geckoStartAcc)
              : RotorRoleRule(roles::CHECKBUTTON);
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }

    if ([key isEqualToString:@"AXStaticTextSearchKey"]) {
      RotorStaticTextRule rule = mImmediateDescendantsOnly
                                     ? RotorStaticTextRule(geckoStartAcc)
                                     : RotorStaticTextRule();
      [matches addObjectsFromArray:[self getMatchesForRule:rule]];
    }
  }

  return matches;
}

- (void)dealloc {
  [mSearchKeys release];
  [super dealloc];
}

@end
