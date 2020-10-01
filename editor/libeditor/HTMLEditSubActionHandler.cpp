/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HTMLEditor.h"

#include <algorithm>
#include <utility>

#include "HTMLEditUtils.h"
#include "WSRunObject.h"
#include "mozilla/Assertions.h"
#include "mozilla/CSSEditUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ContentIterator.h"
#include "mozilla/EditAction.h"
#include "mozilla/EditorDOMPoint.h"
#include "mozilla/EditorUtils.h"
#include "mozilla/InternalMutationEvent.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/OwningNonNull.h"
#include "mozilla/Preferences.h"
#include "mozilla/RangeUtils.h"
#include "mozilla/StaticPrefs_editor.h"  // for StaticPrefs::editor_*
#include "mozilla/TextComposition.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/AncestorIterator.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLBRElement.h"
#include "mozilla/dom/RangeBinding.h"
#include "mozilla/dom/Selection.h"
#include "mozilla/dom/StaticRange.h"
#include "mozilla/mozalloc.h"
#include "nsAString.h"
#include "nsAlgorithm.h"
#include "nsAtom.h"
#include "nsCRT.h"
#include "nsCRTGlue.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsFrameSelection.h"
#include "nsGkAtoms.h"
#include "nsHTMLDocument.h"
#include "nsIContent.h"
#include "nsID.h"
#include "nsIFrame.h"
#include "nsINode.h"
#include "nsLiteralString.h"
#include "nsRange.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsStringFwd.h"
#include "nsStyledElement.h"
#include "nsTArray.h"
#include "nsTextNode.h"
#include "nsThreadUtils.h"
#include "nsUnicharUtils.h"

// Workaround for windows headers
#ifdef SetProp
#  undef SetProp
#endif

class nsISupports;

namespace mozilla {

using namespace dom;
using ChildBlockBoundary = HTMLEditUtils::ChildBlockBoundary;
using InvisibleWhiteSpaces = HTMLEditUtils::InvisibleWhiteSpaces;
using StyleDifference = HTMLEditUtils::StyleDifference;
using TableBoundary = HTMLEditUtils::TableBoundary;

enum { kLonely = 0, kPrevSib = 1, kNextSib = 2, kBothSibs = 3 };

/********************************************************
 *  first some helpful functors we will use
 ********************************************************/

static bool IsStyleCachePreservingSubAction(EditSubAction aEditSubAction) {
  switch (aEditSubAction) {
    case EditSubAction::eDeleteSelectedContent:
    case EditSubAction::eInsertLineBreak:
    case EditSubAction::eInsertParagraphSeparator:
    case EditSubAction::eCreateOrChangeList:
    case EditSubAction::eIndent:
    case EditSubAction::eOutdent:
    case EditSubAction::eSetOrClearAlignment:
    case EditSubAction::eCreateOrRemoveBlock:
    case EditSubAction::eMergeBlockContents:
    case EditSubAction::eRemoveList:
    case EditSubAction::eCreateOrChangeDefinitionListItem:
    case EditSubAction::eInsertElement:
    case EditSubAction::eInsertQuotation:
    case EditSubAction::eInsertQuotedText:
      return true;
    default:
      return false;
  }
}

class MOZ_RAII AutoSetTemporaryAncestorLimiter final {
 public:
  AutoSetTemporaryAncestorLimiter(const HTMLEditor& aHTMLEditor,
                                  Selection& aSelection,
                                  nsINode& aStartPointNode,
                                  AutoRangeArray* aRanges = nullptr) {
    MOZ_ASSERT(aSelection.GetType() == SelectionType::eNormal);

    if (aSelection.GetAncestorLimiter()) {
      return;
    }

    Element* root = aHTMLEditor.FindSelectionRoot(&aStartPointNode);
    if (root) {
      aHTMLEditor.InitializeSelectionAncestorLimit(*root);
      mSelection = &aSelection;
      // Setting ancestor limiter may change ranges which were outer of
      // the new limiter.  Therefore, we need to reinitialize aRanges.
      if (aRanges) {
        aRanges->Initialize(aSelection);
      }
    }
  }

  ~AutoSetTemporaryAncestorLimiter() {
    if (mSelection) {
      mSelection->SetAncestorLimiter(nullptr);
    }
  }

 private:
  RefPtr<Selection> mSelection;
};

// Helper struct for DoJoinNodes() and DoSplitNode().
struct MOZ_STACK_CLASS SavedRange final {
  RefPtr<Selection> mSelection;
  nsCOMPtr<nsINode> mStartContainer;
  nsCOMPtr<nsINode> mEndContainer;
  int32_t mStartOffset = 0;
  int32_t mEndOffset = 0;
};

template void HTMLEditor::SelectBRElementIfCollapsedInEmptyBlock(
    RangeBoundary& aStartRef, RangeBoundary& aEndRef) const;
template void HTMLEditor::SelectBRElementIfCollapsedInEmptyBlock(
    RawRangeBoundary& aStartRef, RangeBoundary& aEndRef) const;
template void HTMLEditor::SelectBRElementIfCollapsedInEmptyBlock(
    RangeBoundary& aStartRef, RawRangeBoundary& aEndRef) const;
template void HTMLEditor::SelectBRElementIfCollapsedInEmptyBlock(
    RawRangeBoundary& aStartRef, RawRangeBoundary& aEndRef) const;
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const RangeBoundary& aStartRef, const RangeBoundary& aEndRef);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const RawRangeBoundary& aStartRef, const RangeBoundary& aEndRef);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const RangeBoundary& aStartRef, const RawRangeBoundary& aEndRef);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const RawRangeBoundary& aStartRef, const RawRangeBoundary& aEndRef);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const RangeBoundary& aStartRef, const RangeBoundary& aEndRef,
    EditSubAction aEditSubAction) const;
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const RawRangeBoundary& aStartRef, const RangeBoundary& aEndRef,
    EditSubAction aEditSubAction) const;
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const RangeBoundary& aStartRef, const RawRangeBoundary& aEndRef,
    EditSubAction aEditSubAction) const;
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const RawRangeBoundary& aStartRef, const RawRangeBoundary& aEndRef,
    EditSubAction aEditSubAction) const;
template EditorDOMPoint HTMLEditor::GetWhiteSpaceEndPoint(
    const RangeBoundary& aPoint, ScanDirection aScanDirection);
template EditorDOMPoint HTMLEditor::GetWhiteSpaceEndPoint(
    const RawRangeBoundary& aPoint, ScanDirection aScanDirection);
template EditorDOMPoint HTMLEditor::GetCurrentHardLineStartPoint(
    const RangeBoundary& aPoint, EditSubAction aEditSubAction) const;
template EditorDOMPoint HTMLEditor::GetCurrentHardLineStartPoint(
    const RawRangeBoundary& aPoint, EditSubAction aEditSubAction) const;
template EditorDOMPoint HTMLEditor::GetCurrentHardLineEndPoint(
    const RangeBoundary& aPoint) const;
template EditorDOMPoint HTMLEditor::GetCurrentHardLineEndPoint(
    const RawRangeBoundary& aPoint) const;
template nsIContent* HTMLEditor::FindNearEditableContent(
    const EditorDOMPoint& aPoint, nsIEditor::EDirection aDirection);
template nsIContent* HTMLEditor::FindNearEditableContent(
    const EditorRawDOMPoint& aPoint, nsIEditor::EDirection aDirection);
template nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPoint& aStartPoint, const EditorDOMPoint& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes);
template nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPointInText& aStartPoint,
    const EditorDOMPointInText& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes);
template Result<bool, nsresult> HTMLEditor::CanMoveOrDeleteSomethingInHardLine(
    const EditorDOMPoint& aPointInHardLine) const;
template Result<bool, nsresult> HTMLEditor::CanMoveOrDeleteSomethingInHardLine(
    const EditorRawDOMPoint& aPointInHardLine) const;

nsresult HTMLEditor::InitEditorContentAndSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsresult rv = TextEditor::InitEditorContentAndSelection();
  if (NS_FAILED(rv)) {
    NS_WARNING("TextEditor::InitEditorContentAndSelection() failed");
    return rv;
  }

  Element* bodyOrDocumentElement = GetRoot();
  if (NS_WARN_IF(!bodyOrDocumentElement && !GetDocument())) {
    return NS_ERROR_FAILURE;
  }

  if (!bodyOrDocumentElement) {
    return NS_OK;
  }

  rv = InsertBRElementToEmptyListItemsAndTableCellsInRange(
      RawRangeBoundary(bodyOrDocumentElement, 0u),
      RawRangeBoundary(bodyOrDocumentElement,
                       bodyOrDocumentElement->GetChildCount()));
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange() "
      "failed, but ignored");
  return NS_OK;
}

void HTMLEditor::OnStartToHandleTopLevelEditSubAction(
    EditSubAction aTopLevelEditSubAction,
    nsIEditor::EDirection aDirectionOfTopLevelEditSubAction, ErrorResult& aRv) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!aRv.Failed());

  EditorBase::OnStartToHandleTopLevelEditSubAction(
      aTopLevelEditSubAction, aDirectionOfTopLevelEditSubAction, aRv);

  MOZ_ASSERT(GetTopLevelEditSubAction() == aTopLevelEditSubAction);
  MOZ_ASSERT(GetDirectionOfTopLevelEditSubAction() ==
             aDirectionOfTopLevelEditSubAction);

  if (NS_WARN_IF(Destroyed())) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  if (!mInitSucceeded) {
    return;  // We should do nothing if we're being initialized.
  }

  NS_WARNING_ASSERTION(
      !aRv.Failed(),
      "EditorBase::OnStartToHandleTopLevelEditSubAction() failed");

  // Remember where our selection was before edit action took place:
  if (GetCompositionStartPoint().IsSet()) {
    // If there is composition string, let's remember current composition
    // range.
    TopLevelEditSubActionDataRef().mSelectedRange->StoreRange(
        GetCompositionStartPoint(), GetCompositionEndPoint());
  } else {
    // Get the selection location
    // XXX This may occur so that I think that we shouldn't throw exception
    //     in this case.
    if (NS_WARN_IF(!SelectionRefPtr()->RangeCount())) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return;
    }
    if (const nsRange* range = SelectionRefPtr()->GetRangeAt(0)) {
      TopLevelEditSubActionDataRef().mSelectedRange->StoreRange(*range);
    }
  }

  // Register with range updater to track this as we perturb the doc
  RangeUpdaterRef().RegisterRangeItem(
      *TopLevelEditSubActionDataRef().mSelectedRange);

  // Remember current inline styles for deletion and normal insertion ops
  bool cacheInlineStyles;
  switch (aTopLevelEditSubAction) {
    case EditSubAction::eInsertText:
    case EditSubAction::eInsertTextComingFromIME:
    case EditSubAction::eDeleteSelectedContent:
      cacheInlineStyles = true;
      break;
    default:
      cacheInlineStyles =
          IsStyleCachePreservingSubAction(aTopLevelEditSubAction);
      break;
  }
  if (cacheInlineStyles) {
    nsCOMPtr<nsIContent> containerContent = nsIContent::FromNodeOrNull(
        aDirectionOfTopLevelEditSubAction == nsIEditor::eNext
            ? TopLevelEditSubActionDataRef().mSelectedRange->mEndContainer
            : TopLevelEditSubActionDataRef().mSelectedRange->mStartContainer);
    if (NS_WARN_IF(!containerContent)) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    nsresult rv = CacheInlineStyles(*containerContent);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::CacheInlineStyles() failed");
      aRv.Throw(rv);
      return;
    }
  }

  // Stabilize the document against contenteditable count changes
  Document* document = GetDocument();
  if (NS_WARN_IF(!document)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  if (document->GetEditingState() == Document::EditingState::eContentEditable) {
    document->ChangeContentEditableCount(nullptr, +1);
    TopLevelEditSubActionDataRef().mRestoreContentEditableCount = true;
  }

  // Check that selection is in subtree defined by body node
  nsresult rv = EnsureSelectionInBodyOrDocumentElement();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::EnsureSelectionInBodyOrDocumentElement() "
                       "failed, but ignored");
}

nsresult HTMLEditor::OnEndHandlingTopLevelEditSubAction() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv;
  while (true) {
    if (NS_WARN_IF(Destroyed())) {
      rv = NS_ERROR_EDITOR_DESTROYED;
      break;
    }

    if (!mInitSucceeded) {
      rv = NS_OK;  // We should do nothing if we're being initialized.
      break;
    }

    // Do all the tricky stuff
    rv = OnEndHandlingTopLevelEditSubActionInternal();
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::OnEndHandlingTopLevelEditSubActionInternal() failied");
    // Perhaps, we need to do the following jobs even if the editor has been
    // destroyed since they adjust some states of HTML document but don't
    // modify the DOM tree nor Selection.

    // Free up selectionState range item
    if (TopLevelEditSubActionDataRef().mSelectedRange) {
      RangeUpdaterRef().DropRangeItem(
          *TopLevelEditSubActionDataRef().mSelectedRange);
    }

    // Reset the contenteditable count to its previous value
    if (TopLevelEditSubActionDataRef().mRestoreContentEditableCount) {
      Document* document = GetDocument();
      if (NS_WARN_IF(!document)) {
        rv = NS_ERROR_FAILURE;
        break;
      }
      if (document->GetEditingState() ==
          Document::EditingState::eContentEditable) {
        document->ChangeContentEditableCount(nullptr, -1);
      }
    }
    break;
  }
  DebugOnly<nsresult> rvIgnored =
      EditorBase::OnEndHandlingTopLevelEditSubAction();
  NS_WARNING_ASSERTION(
      NS_FAILED(rv) || NS_SUCCEEDED(rvIgnored),
      "EditorBase::OnEndHandlingTopLevelEditSubAction() failed, but ignored");
  MOZ_ASSERT(!GetTopLevelEditSubAction());
  MOZ_ASSERT(GetDirectionOfTopLevelEditSubAction() == eNone);
  return rv;
}

nsresult HTMLEditor::OnEndHandlingTopLevelEditSubActionInternal() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv = EnsureSelectionInBodyOrDocumentElement();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::EnsureSelectionInBodyOrDocumentElement() "
                       "failed, but ignored");

  switch (GetTopLevelEditSubAction()) {
    case EditSubAction::eReplaceHeadWithHTMLSource:
    case EditSubAction::eCreatePaddingBRElementForEmptyEditor:
      return NS_OK;
    default:
      break;
  }

  if (TopLevelEditSubActionDataRef().mChangedRange->IsPositioned() &&
      GetTopLevelEditSubAction() != EditSubAction::eUndo &&
      GetTopLevelEditSubAction() != EditSubAction::eRedo) {
    // don't let any txns in here move the selection around behind our back.
    // Note that this won't prevent explicit selection setting from working.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eInsertLineBreak:
      case EditSubAction::eInsertParagraphSeparator:
      case EditSubAction::eDeleteText: {
        // XXX We should investigate whether this is really needed because it
        //     seems that the following code does not handle the white-spaces.
        RefPtr<nsRange> extendedChangedRange =
            CreateRangeIncludingAdjuscentWhiteSpaces(
                *TopLevelEditSubActionDataRef().mChangedRange);
        if (extendedChangedRange) {
          MOZ_ASSERT(extendedChangedRange->IsPositioned());
          // Use extended range temporarily.
          TopLevelEditSubActionDataRef().mChangedRange =
              std::move(extendedChangedRange);
        }
        break;
      }
      default: {
        RefPtr<nsRange> extendedChangedRange =
            CreateRangeExtendedToHardLineStartAndEnd(
                *TopLevelEditSubActionDataRef().mChangedRange,
                GetTopLevelEditSubAction());
        if (extendedChangedRange) {
          MOZ_ASSERT(extendedChangedRange->IsPositioned());
          // Use extended range temporarily.
          TopLevelEditSubActionDataRef().mChangedRange =
              std::move(extendedChangedRange);
        }
        break;
      }
    }

    // if we did a ranged deletion or handling backspace key, make sure we have
    // a place to put caret.
    // Note we only want to do this if the overall operation was deletion,
    // not if deletion was done along the way for
    // EditSubAction::eInsertHTMLSource, EditSubAction::eInsertText, etc.
    // That's why this is here rather than DeleteSelectionAsSubAction().
    // However, we shouldn't insert <br> elements if we've already removed
    // empty block parents because users may want to disappear the line by
    // the deletion.
    // XXX We should make HandleDeleteSelection() store expected container
    //     for handling this here since we cannot trust current selection is
    //     collapsed at deleted point.
    if (GetTopLevelEditSubAction() == EditSubAction::eDeleteSelectedContent &&
        TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange &&
        !TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks) {
      nsresult rv = InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
          EditorBase::GetStartPoint(*SelectionRefPtr()));
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::"
            "InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() "
            "failed");
        return rv;
      }
    }

    // add in any needed <br>s, and remove any unneeded ones.
    nsresult rv = InsertBRElementToEmptyListItemsAndTableCellsInRange(
        TopLevelEditSubActionDataRef().mChangedRange->StartRef().AsRaw(),
        TopLevelEditSubActionDataRef().mChangedRange->EndRef().AsRaw());
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange()"
        " failed, but ignored");

    // merge any adjacent text nodes
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
        break;
      default: {
        nsresult rv = CollapseAdjacentTextNodes(
            MOZ_KnownLive(*TopLevelEditSubActionDataRef().mChangedRange));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::CollapseAdjacentTextNodes() failed");
          return rv;
        }
        break;
      }
    }

    // clean up any empty nodes in the selection
    rv = RemoveEmptyNodesIn(
        MOZ_KnownLive(*TopLevelEditSubActionDataRef().mChangedRange));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::RemoveEmptyNodesIn() failed");
      return rv;
    }

    // attempt to transform any unneeded nbsp's into spaces after doing various
    // operations
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eDeleteSelectedContent:
        if (TopLevelEditSubActionDataRef().mDidNormalizeWhitespaces) {
          break;
        }
        [[fallthrough]];
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eInsertLineBreak:
      case EditSubAction::eInsertParagraphSeparator:
      case EditSubAction::ePasteHTMLContent:
      case EditSubAction::eInsertHTMLSource: {
        // TODO: Temporarily, WhiteSpaceVisibilityKeeper replaces ASCII
        //       white-spaces with NPSPs and then, we'll replace them with ASCII
        //       white-spaces here.  We should avoid this overwriting things as
        //       far as possible because replacing characters in text nodes
        //       causes running mutation event listeners which are really
        //       expensive.
        // Adjust end of composition string if there is composition string.
        EditorRawDOMPoint pointToAdjust(GetCompositionEndPoint());
        if (!pointToAdjust.IsSet()) {
          // Otherwise, adjust current selection start point.
          pointToAdjust = EditorBase::GetStartPoint(*SelectionRefPtr());
          if (NS_WARN_IF(!pointToAdjust.IsSet())) {
            return NS_ERROR_FAILURE;
          }
        }
        rv = WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
            *this, pointToAdjust);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
              "failed");
          return rv;
        }

        // also do this for original selection endpoints.
        // XXX Hmm, if `NormalizeVisibleWhiteSpacesAt()` runs mutation event
        //     listener and that causes changing `mSelectedRange`, what we
        //     should do?
        if (NS_WARN_IF(
                !TopLevelEditSubActionDataRef().mSelectedRange->IsSet())) {
          return NS_ERROR_FAILURE;
        }
        rv = WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
            *this,
            TopLevelEditSubActionDataRef().mSelectedRange->StartRawPoint());
        if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
            "failed, but ignored");
        // we only need to handle old selection endpoint if it was different
        // from start
        if (TopLevelEditSubActionDataRef().mSelectedRange->IsCollapsed()) {
          nsresult rv =
              WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
                  *this,
                  TopLevelEditSubActionDataRef().mSelectedRange->EndRawPoint());
          if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
              "failed, but ignored");
        }
        break;
      }
      default:
        break;
    }

    // If we created a new block, make sure caret is in it.
    if (TopLevelEditSubActionDataRef().mNewBlockElement &&
        SelectionRefPtr()->IsCollapsed()) {
      nsresult rv = EnsureCaretInBlockElement(
          MOZ_KnownLive(*TopLevelEditSubActionDataRef().mNewBlockElement));
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::EnsureSelectionInBlockElement() failed, but ignored");
    }

    // Adjust selection for insert text, html paste, and delete actions if
    // we haven't removed new empty blocks.  Note that if empty block parents
    // are removed, Selection should've been adjusted by the method which
    // did it.
    if (!TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks &&
        SelectionRefPtr()->IsCollapsed()) {
      switch (GetTopLevelEditSubAction()) {
        case EditSubAction::eInsertText:
        case EditSubAction::eInsertTextComingFromIME:
        case EditSubAction::eDeleteSelectedContent:
        case EditSubAction::eInsertLineBreak:
        case EditSubAction::eInsertParagraphSeparator:
        case EditSubAction::ePasteHTMLContent:
        case EditSubAction::eInsertHTMLSource:
          // XXX AdjustCaretPositionAndEnsurePaddingBRElement() intentionally
          //     does not create padding `<br>` element for empty editor.
          //     Investigate which is better that whether this should does it
          //     or wait MaybeCreatePaddingBRElementForEmptyEditor().
          rv = AdjustCaretPositionAndEnsurePaddingBRElement(
              GetDirectionOfTopLevelEditSubAction());
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "HTMLEditor::AdjustCaretPositionAndEnsurePaddingBRElement() "
                "failed");
            return rv;
          }
          break;
        default:
          break;
      }
    }

    // check for any styles which were removed inappropriately
    bool reapplyCachedStyle;
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eDeleteSelectedContent:
        reapplyCachedStyle = true;
        break;
      default:
        reapplyCachedStyle =
            IsStyleCachePreservingSubAction(GetTopLevelEditSubAction());
        break;
    }
    if (reapplyCachedStyle) {
      DebugOnly<nsresult> rvIgnored =
          mTypeInState->UpdateSelState(SelectionRefPtr());
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "TypeInState::UpdateSelState() failed, but ignored");
      rv = ReapplyCachedStyles();
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::ReapplyCachedStyles() failed");
        return rv;
      }
      TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
    }
  }

  rv = HandleInlineSpellCheck(
      TopLevelEditSubActionDataRef().mSelectedRange->StartPoint(),
      TopLevelEditSubActionDataRef().mChangedRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::HandleInlineSpellCheck() failed");
    return rv;
  }

  // detect empty doc
  // XXX Need to investigate when the padding <br> element is removed because
  //     I don't see the <br> element with testing manually.  If it won't be
  //     used, we can get rid of this cost.
  rv = MaybeCreatePaddingBRElementForEmptyEditor();
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "EditorBase::MaybeCreatePaddingBRElementForEmptyEditor() failed");
    return rv;
  }

  // adjust selection HINT if needed
  if (!TopLevelEditSubActionDataRef().mDidExplicitlySetInterLine &&
      SelectionRefPtr()->IsCollapsed()) {
    SetSelectionInterlinePosition();
  }

  return NS_OK;
}

EditActionResult HTMLEditor::CanHandleHTMLEditSubAction() const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }

  // If there is not selection ranges, we should ignore the result.
  if (!SelectionRefPtr()->RangeCount()) {
    return EditActionCanceled();
  }

  const nsRange* range = SelectionRefPtr()->GetRangeAt(0);
  nsINode* selStartNode = range->GetStartContainer();
  if (NS_WARN_IF(!selStartNode)) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  if (!HTMLEditUtils::IsSimplyEditableNode(*selStartNode)) {
    return EditActionCanceled();
  }

  nsINode* selEndNode = range->GetEndContainer();
  if (NS_WARN_IF(!selEndNode)) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  if (selStartNode == selEndNode) {
    return EditActionIgnored();
  }

  if (!HTMLEditUtils::IsSimplyEditableNode(*selEndNode)) {
    return EditActionCanceled();
  }

  // XXX What does it mean the common ancestor is editable?  I have no idea.
  //     It should be in same (active) editing host, and even if it's editable,
  //     there may be non-editable contents in the range.
  nsINode* commonAncestor = range->GetClosestCommonInclusiveAncestor();
  if (!commonAncestor) {
    NS_WARNING(
        "AbstractRange::GetClosestCommonInclusiveAncestor() returned nullptr");
    return EditActionResult(NS_ERROR_FAILURE);
  }
  return HTMLEditUtils::IsSimplyEditableNode(*commonAncestor)
             ? EditActionIgnored()
             : EditActionCanceled();
}

ListElementSelectionState::ListElementSelectionState(HTMLEditor& aHTMLEditor,
                                                     ErrorResult& aRv) {
  MOZ_ASSERT(!aRv.Failed());

  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  // XXX Should we create another constructor which won't create
  //     AutoEditActionDataSetter?  Or should we create another
  //     AutoEditActionDataSetter which won't nest edit action?
  EditorBase::AutoEditActionDataSetter editActionData(aHTMLEditor,
                                                      EditAction::eNotEditing);
  if (NS_WARN_IF(!editActionData.CanHandle())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = aHTMLEditor.CollectEditTargetNodesInExtendedSelectionRanges(
      arrayOfContents, EditSubAction::eCreateOrChangeList,
      HTMLEditor::CollectNonEditableNodes::No);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::CollectEditTargetNodesInExtendedSelectionRanges("
        "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
    aRv = EditorBase::ToGenericNSResult(rv);
    return;
  }

  // Examine list type for nodes in selection.
  for (const auto& content : arrayOfContents) {
    if (!content->IsElement()) {
      mIsOtherContentSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::ul)) {
      mIsULElementSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::ol)) {
      mIsOLElementSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::li)) {
      if (Element* parent = content->GetParentElement()) {
        if (parent->IsHTMLElement(nsGkAtoms::ul)) {
          mIsULElementSelected = true;
        } else if (parent->IsHTMLElement(nsGkAtoms::ol)) {
          mIsOLElementSelected = true;
        }
      }
    } else if (content->IsAnyOfHTMLElements(nsGkAtoms::dl, nsGkAtoms::dt,
                                            nsGkAtoms::dd)) {
      mIsDLElementSelected = true;
    } else {
      mIsOtherContentSelected = true;
    }

    if (mIsULElementSelected && mIsOLElementSelected && mIsDLElementSelected &&
        mIsOtherContentSelected) {
      break;
    }
  }
}

ListItemElementSelectionState::ListItemElementSelectionState(
    HTMLEditor& aHTMLEditor, ErrorResult& aRv) {
  MOZ_ASSERT(!aRv.Failed());

  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  // XXX Should we create another constructor which won't create
  //     AutoEditActionDataSetter?  Or should we create another
  //     AutoEditActionDataSetter which won't nest edit action?
  EditorBase::AutoEditActionDataSetter editActionData(aHTMLEditor,
                                                      EditAction::eNotEditing);
  if (NS_WARN_IF(!editActionData.CanHandle())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = aHTMLEditor.CollectEditTargetNodesInExtendedSelectionRanges(
      arrayOfContents, EditSubAction::eCreateOrChangeList,
      HTMLEditor::CollectNonEditableNodes::No);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::CollectEditTargetNodesInExtendedSelectionRanges("
        "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
    aRv = EditorBase::ToGenericNSResult(rv);
    return;
  }

  // examine list type for nodes in selection
  for (const auto& content : arrayOfContents) {
    if (!content->IsElement()) {
      mIsOtherElementSelected = true;
    } else if (content->IsAnyOfHTMLElements(nsGkAtoms::ul, nsGkAtoms::ol,
                                            nsGkAtoms::li)) {
      mIsLIElementSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::dt)) {
      mIsDTElementSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::dd)) {
      mIsDDElementSelected = true;
    } else if (content->IsHTMLElement(nsGkAtoms::dl)) {
      if (mIsDTElementSelected && mIsDDElementSelected) {
        continue;
      }
      // need to look inside dl and see which types of items it has
      DefinitionListItemScanner scanner(*content->AsElement());
      mIsDTElementSelected |= scanner.DTElementFound();
      mIsDDElementSelected |= scanner.DDElementFound();
    } else {
      mIsOtherElementSelected = true;
    }

    if (mIsLIElementSelected && mIsDTElementSelected && mIsDDElementSelected &&
        mIsOtherElementSelected) {
      break;
    }
  }
}

AlignStateAtSelection::AlignStateAtSelection(HTMLEditor& aHTMLEditor,
                                             ErrorResult& aRv) {
  MOZ_ASSERT(!aRv.Failed());

  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  // XXX Should we create another constructor which won't create
  //     AutoEditActionDataSetter?  Or should we create another
  //     AutoEditActionDataSetter which won't nest edit action?
  EditorBase::AutoEditActionDataSetter editActionData(aHTMLEditor,
                                                      EditAction::eNotEditing);
  if (NS_WARN_IF(!editActionData.CanHandle())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  if (aHTMLEditor.IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return;
  }

  // For now, just return first alignment.  We don't check if it's mixed.
  // This is for efficiency given that our current UI doesn't care if it's
  // mixed.
  // cmanske: NOT TRUE! We would like to pay attention to mixed state in
  // [Format] -> [Align] submenu!

  // This routine assumes that alignment is done ONLY by `<div>` elements
  // if aHTMLEditor is not in CSS mode.

  if (NS_WARN_IF(!aHTMLEditor.GetRoot())) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  OwningNonNull<Element> bodyOrDocumentElement = *aHTMLEditor.GetRoot();
  EditorRawDOMPoint atBodyOrDocumentElement(bodyOrDocumentElement);

  const nsRange* firstRange = aHTMLEditor.SelectionRefPtr()->GetRangeAt(0);
  mFoundSelectionRanges = !!firstRange;
  if (!mFoundSelectionRanges) {
    NS_WARNING("There was no selection range");
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  EditorRawDOMPoint atStartOfSelection(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

  nsIContent* editTargetContent = nullptr;
  // If selection is collapsed or in a text node, take the container.
  if (aHTMLEditor.SelectionRefPtr()->IsCollapsed() ||
      atStartOfSelection.IsInTextNode()) {
    editTargetContent = atStartOfSelection.GetContainerAsContent();
    if (NS_WARN_IF(!editTargetContent)) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
  }
  // If selection container is the `<body>` element which is set to
  // `HTMLDocument.body`, take first editable node in it.
  // XXX Why don't we just compare `atStartOfSelection.GetChild()` and
  //     `bodyOrDocumentElement`?  Then, we can avoid computing the
  //     offset.
  else if (atStartOfSelection.IsContainerHTMLElement(nsGkAtoms::html) &&
           atBodyOrDocumentElement.IsSet() &&
           atStartOfSelection.Offset() == atBodyOrDocumentElement.Offset()) {
    editTargetContent = aHTMLEditor.GetNextEditableNode(atStartOfSelection);
    if (NS_WARN_IF(!editTargetContent)) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
  }
  // Otherwise, use first selected node.
  // XXX Only for retreiving it, the following block treats all selected
  //     ranges.  `HTMLEditor` should have
  //     `GetFirstSelectionRangeExtendedToHardLineStartAndEnd()`.
  else {
    AutoTArray<RefPtr<nsRange>, 4> arrayOfRanges;
    aHTMLEditor.GetSelectionRangesExtendedToHardLineStartAndEnd(
        arrayOfRanges, EditSubAction::eSetOrClearAlignment);

    AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
    nsresult rv = aHTMLEditor.CollectEditTargetNodes(
        arrayOfRanges, arrayOfContents, EditSubAction::eSetOrClearAlignment,
        HTMLEditor::CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::CollectEditTargetNodes(eSetOrClearAlignment, "
          "CollectNonEditableNodes::Yes) failed");
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    if (arrayOfContents.IsEmpty()) {
      NS_WARNING(
          "HTMLEditor::CollectEditTargetNodes(eSetOrClearAlignment, "
          "CollectNonEditableNodes::Yes) returned no contents");
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    editTargetContent = arrayOfContents[0];
  }

  RefPtr<Element> blockElementAtEditTarget =
      HTMLEditUtils::GetInclusiveAncestorBlockElement(*editTargetContent);
  if (NS_WARN_IF(!blockElementAtEditTarget)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  if (aHTMLEditor.IsCSSEnabled() &&
      CSSEditUtils::IsCSSEditableProperty(blockElementAtEditTarget, nullptr,
                                          nsGkAtoms::align)) {
    // We are in CSS mode and we know how to align this element with CSS
    nsAutoString value;
    // Let's get the value(s) of text-align or margin-left/margin-right
    DebugOnly<nsresult> rvIgnored =
        CSSEditUtils::GetComputedCSSEquivalentToHTMLInlineStyleSet(
            *blockElementAtEditTarget, nullptr, nsGkAtoms::align, value);
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
      return;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rvIgnored),
        "CSSEditUtils::GetComputedCSSEquivalentToHTMLInlineStyleSet(nsGkAtoms::"
        "align, "
        "eComputed) failed, but ignored");
    if (value.EqualsLiteral("center") || value.EqualsLiteral("-moz-center") ||
        value.EqualsLiteral("auto auto")) {
      mFirstAlign = nsIHTMLEditor::eCenter;
      return;
    }
    if (value.EqualsLiteral("right") || value.EqualsLiteral("-moz-right") ||
        value.EqualsLiteral("auto 0px")) {
      mFirstAlign = nsIHTMLEditor::eRight;
      return;
    }
    if (value.EqualsLiteral("justify")) {
      mFirstAlign = nsIHTMLEditor::eJustify;
      return;
    }
    // XXX In RTL document, is this expected?
    mFirstAlign = nsIHTMLEditor::eLeft;
    return;
  }

  for (nsIContent* containerContent :
       editTargetContent->InclusiveAncestorsOfType<nsIContent>()) {
    // If the node is a parent `<table>` element of edit target, let's break
    // here to materialize the 'inline-block' behaviour of html tables
    // regarding to text alignment.
    if (containerContent != editTargetContent &&
        containerContent->IsHTMLElement(nsGkAtoms::table)) {
      return;
    }

    if (CSSEditUtils::IsCSSEditableProperty(containerContent, nullptr,
                                            nsGkAtoms::align)) {
      nsAutoString value;
      DebugOnly<nsresult> rvIgnored = CSSEditUtils::GetSpecifiedProperty(
          *containerContent, *nsGkAtoms::textAlign, value);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "CSSEditUtils::GetSpecifiedProperty(nsGkAtoms::"
                           "textAlign) failed, but ignored");
      if (!value.IsEmpty()) {
        if (value.EqualsLiteral("center")) {
          mFirstAlign = nsIHTMLEditor::eCenter;
          return;
        }
        if (value.EqualsLiteral("right")) {
          mFirstAlign = nsIHTMLEditor::eRight;
          return;
        }
        if (value.EqualsLiteral("justify")) {
          mFirstAlign = nsIHTMLEditor::eJustify;
          return;
        }
        if (value.EqualsLiteral("left")) {
          mFirstAlign = nsIHTMLEditor::eLeft;
          return;
        }
        // XXX
        // text-align: start and end aren't supported yet
      }
    }

    if (!HTMLEditUtils::SupportsAlignAttr(*containerContent)) {
      continue;
    }

    nsAutoString alignAttributeValue;
    containerContent->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::align,
                                           alignAttributeValue);
    if (alignAttributeValue.IsEmpty()) {
      continue;
    }

    if (alignAttributeValue.LowerCaseEqualsASCII("center")) {
      mFirstAlign = nsIHTMLEditor::eCenter;
      return;
    }
    if (alignAttributeValue.LowerCaseEqualsASCII("right")) {
      mFirstAlign = nsIHTMLEditor::eRight;
      return;
    }
    // XXX This is odd case.  `<div align="justify">` is not in any standards.
    if (alignAttributeValue.LowerCaseEqualsASCII("justify")) {
      mFirstAlign = nsIHTMLEditor::eJustify;
      return;
    }
    // XXX In RTL document, is this expected?
    mFirstAlign = nsIHTMLEditor::eLeft;
    return;
  }
}

MOZ_CAN_RUN_SCRIPT static nsStaticAtom& MarginPropertyAtomForIndent(
    nsIContent& aContent) {
  nsAutoString direction;
  DebugOnly<nsresult> rvIgnored = CSSEditUtils::GetComputedProperty(
      aContent, *nsGkAtoms::direction, direction);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "CSSEditUtils::GetComputedProperty(nsGkAtoms::direction)"
                       " failed, but ignored");
  return direction.EqualsLiteral("rtl") ? *nsGkAtoms::marginRight
                                        : *nsGkAtoms::marginLeft;
}

ParagraphStateAtSelection::ParagraphStateAtSelection(HTMLEditor& aHTMLEditor,
                                                     ErrorResult& aRv) {
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  // XXX Should we create another constructor which won't create
  //     AutoEditActionDataSetter?  Or should we create another
  //     AutoEditActionDataSetter which won't nest edit action?
  EditorBase::AutoEditActionDataSetter editActionData(aHTMLEditor,
                                                      EditAction::eNotEditing);
  if (NS_WARN_IF(!editActionData.CanHandle())) {
    aRv = EditorBase::ToGenericNSResult(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  if (aHTMLEditor.IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv =
      CollectEditableFormatNodesInSelection(aHTMLEditor, arrayOfContents);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "ParagraphStateAtSelection::CollectEditableFormatNodesInSelection() "
        "failed");
    aRv.Throw(rv);
    return;
  }

  // We need to append descendant format block if block nodes are not format
  // block.  This is so we only have to look "up" the hierarchy to find
  // format nodes, instead of both up and down.
  for (int32_t i = arrayOfContents.Length() - 1; i >= 0; i--) {
    auto& content = arrayOfContents[i];
    nsAutoString format;
    if (HTMLEditUtils::IsBlockElement(content) &&
        !HTMLEditUtils::IsFormatNode(content)) {
      // XXX This RemoveObject() call has already been commented out and
      //     the above comment explained we're trying to replace non-format
      //     block nodes in the array.  According to the following blocks and
      //     `AppendDescendantFormatNodesAndFirstInlineNode()`, replacing
      //     non-format block with descendants format blocks makes sense.
      // arrayOfContents.RemoveObject(node);
      ParagraphStateAtSelection::AppendDescendantFormatNodesAndFirstInlineNode(
          arrayOfContents, *content->AsElement());
    }
  }

  // We might have an empty node list.  if so, find selection parent
  // and put that on the list
  if (arrayOfContents.IsEmpty()) {
    EditorRawDOMPoint atCaret(
        EditorBase::GetStartPoint(*aHTMLEditor.SelectionRefPtr()));
    if (NS_WARN_IF(!atCaret.IsSet())) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    nsIContent* content = atCaret.GetContainerAsContent();
    if (NS_WARN_IF(!content)) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    arrayOfContents.AppendElement(*content);
  }

  Element* bodyOrDocumentElement = aHTMLEditor.GetRoot();
  if (NS_WARN_IF(!bodyOrDocumentElement)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  for (auto& content : Reversed(arrayOfContents)) {
    nsAtom* paragraphStateOfNode = nsGkAtoms::_empty;
    if (HTMLEditUtils::IsFormatNode(content)) {
      MOZ_ASSERT(content->NodeInfo()->NameAtom());
      paragraphStateOfNode = content->NodeInfo()->NameAtom();
    }
    // Ignore non-format block node since its children have been appended
    // the list above so that we'll handle this descendants later.
    else if (HTMLEditUtils::IsBlockElement(content)) {
      continue;
    }
    // If we meet an inline node, let's get its parent format.
    else {
      for (nsINode* parentNode = content->GetParentNode(); parentNode;
           parentNode = parentNode->GetParentNode()) {
        // If we reach `HTMLDocument.body` or `Document.documentElement`,
        // there is no format.
        if (parentNode == bodyOrDocumentElement) {
          break;
        }
        if (HTMLEditUtils::IsFormatNode(parentNode)) {
          MOZ_ASSERT(parentNode->NodeInfo()->NameAtom());
          paragraphStateOfNode = parentNode->NodeInfo()->NameAtom();
          break;
        }
      }
    }

    // if this is the first node, we've found, remember it as the format
    if (!mFirstParagraphState) {
      mFirstParagraphState = paragraphStateOfNode;
      continue;
    }
    // else make sure it matches previously found format
    if (mFirstParagraphState != paragraphStateOfNode) {
      mIsMixed = true;
      break;
    }
  }
}

// static
void ParagraphStateAtSelection::AppendDescendantFormatNodesAndFirstInlineNode(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    Element& aNonFormatBlockElement) {
  MOZ_ASSERT(HTMLEditUtils::IsBlockElement(aNonFormatBlockElement));
  MOZ_ASSERT(!HTMLEditUtils::IsFormatNode(&aNonFormatBlockElement));

  // We only need to place any one inline inside this node onto
  // the list.  They are all the same for purposes of determining
  // paragraph style.  We use foundInline to track this as we are
  // going through the children in the loop below.
  bool foundInline = false;
  for (nsIContent* childContent = aNonFormatBlockElement.GetFirstChild();
       childContent; childContent = childContent->GetNextSibling()) {
    bool isBlock = HTMLEditUtils::IsBlockElement(*childContent);
    bool isFormat = HTMLEditUtils::IsFormatNode(childContent);
    // If the child is a non-format block element, let's check its children
    // recursively.
    if (isBlock && !isFormat) {
      ParagraphStateAtSelection::AppendDescendantFormatNodesAndFirstInlineNode(
          aArrayOfContents, *childContent->AsElement());
      continue;
    }

    // If it's a format block, append it.
    if (isFormat) {
      aArrayOfContents.AppendElement(*childContent);
      continue;
    }

    MOZ_ASSERT(!isBlock);

    // If we haven't found inline node, append only this first inline node.
    // XXX I think that this makes sense if caller of this removes
    //     aNonFormatBlockElement from aArrayOfContents because the last loop
    //     of the constructor can check parent format block with
    //     aNonFormatBlockElement.
    if (!foundInline) {
      foundInline = true;
      aArrayOfContents.AppendElement(*childContent);
      continue;
    }
  }
}

// static
nsresult ParagraphStateAtSelection::CollectEditableFormatNodesInSelection(
    HTMLEditor& aHTMLEditor,
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents) {
  nsresult rv = aHTMLEditor.CollectEditTargetNodesInExtendedSelectionRanges(
      aArrayOfContents, EditSubAction::eCreateOrRemoveBlock,
      HTMLEditor::CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::CollectEditTargetNodesInExtendedSelectionRanges("
        "eCreateOrRemoveBlock, CollectNonEditableNodes::Yes) failed");
    return rv;
  }

  // Pre-process our list of nodes
  for (int32_t i = aArrayOfContents.Length() - 1; i >= 0; i--) {
    OwningNonNull<nsIContent> content = aArrayOfContents[i];

    // Remove all non-editable nodes.  Leave them be.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      aArrayOfContents.RemoveElementAt(i);
      continue;
    }

    // Scan for table elements.  If we find table elements other than table,
    // replace it with a list of any editable non-table content.  Ditto for
    // list elements.
    if (HTMLEditUtils::IsAnyTableElement(content) ||
        HTMLEditUtils::IsAnyListElement(content) ||
        HTMLEditUtils::IsListItem(content)) {
      aArrayOfContents.RemoveElementAt(i);
      aHTMLEditor.CollectChildren(content, aArrayOfContents, i,
                                  HTMLEditor::CollectListChildren::Yes,
                                  HTMLEditor::CollectTableChildren::Yes,
                                  HTMLEditor::CollectNonEditableNodes::Yes);
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::EnsureCaretNotAfterPaddingBRElement() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRefPtr()->IsCollapsed());

  // If we are after a padding `<br>` element for empty last line in the same
  // block, then move selection to be before it
  const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }

  EditorRawDOMPoint atSelectionStart(firstRange->StartRef());
  if (NS_WARN_IF(!atSelectionStart.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  MOZ_ASSERT(atSelectionStart.IsSetAndValid());

  nsCOMPtr<nsIContent> previousEditableContent =
      GetPreviousEditableHTMLNode(atSelectionStart);
  if (!previousEditableContent ||
      !EditorUtils::IsPaddingBRElementForEmptyLastLine(
          *previousEditableContent)) {
    return NS_OK;
  }

  if (!atSelectionStart.IsInContentNode()) {
    return NS_OK;
  }

  RefPtr<Element> blockElementAtSelectionStart =
      HTMLEditUtils::GetInclusiveAncestorBlockElement(
          *atSelectionStart.ContainerAsContent());
  RefPtr<Element> parentBlockElementOfPreviousEditableContent =
      HTMLEditUtils::GetAncestorBlockElement(*previousEditableContent);

  if (!blockElementAtSelectionStart ||
      blockElementAtSelectionStart !=
          parentBlockElementOfPreviousEditableContent) {
    return NS_OK;
  }

  // If we are here then the selection is right after a padding <br>
  // element for empty last line that is in the same block as the
  // selection.  We need to move the selection start to be before the
  // padding <br> element.
  EditorRawDOMPoint atPreviousEditableContent(previousEditableContent);
  nsresult rv = CollapseSelectionTo(atPreviousEditableContent);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

nsresult HTMLEditor::PrepareInlineStylesForCaret() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(SelectionRefPtr()->IsCollapsed());

  // XXX This method works with the top level edit sub-action, but this
  //     must be wrong if we are handling nested edit action.

  if (TopLevelEditSubActionDataRef().mDidDeleteSelection) {
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eDeleteSelectedContent: {
        nsresult rv = ReapplyCachedStyles();
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReapplyCachedStyles() failed");
          return rv;
        }
        break;
      }
      default:
        break;
    }
  }
  // For most actions we want to clear the cached styles, but there are
  // exceptions
  if (!IsStyleCachePreservingSubAction(GetTopLevelEditSubAction())) {
    TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
  }
  return NS_OK;
}

EditActionResult HTMLEditor::HandleInsertText(
    EditSubAction aEditSubAction, const nsAString& aInsertionString) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(aEditSubAction == EditSubAction::eInsertText ||
             aEditSubAction == EditSubAction::eInsertTextComingFromIME);

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  UndefineCaretBidiLevel();

  // If the selection isn't collapsed, delete it.  Don't delete existing inline
  // tags, because we're hopefully going to insert text (bug 787432).
  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv =
        DeleteSelectionAsSubAction(nsIEditor::eNone, nsIEditor::eNoStrip);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::DeleteSelectionAsSubAction(nsIEditor::eNone, "
          "nsIEditor::eNoStrip) failed");
      return EditActionHandled(rv);
    }
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Document> document = GetDocument();
  if (NS_WARN_IF(!document)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  RefPtr<const nsRange> firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  // for every property that is set, insert a new inline style node
  // XXX CreateStyleForInsertText() adjusts selection automatically, but
  //     it should just return the insertion point instead.
  rv = CreateStyleForInsertText(*firstRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::CreateStyleForInsertText() failed");
    return EditActionHandled(rv);
  }

  firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  EditorDOMPoint pointToInsert(firstRange->StartRef());
  if (NS_WARN_IF(!pointToInsert.IsSet()) ||
      NS_WARN_IF(!pointToInsert.IsInContentNode())) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  MOZ_ASSERT(pointToInsert.IsSetAndValid());

  // dont put text in places that can't have it
  if (!pointToInsert.IsInTextNode() &&
      !HTMLEditUtils::CanNodeContain(*pointToInsert.GetContainer(),
                                     *nsGkAtoms::textTagName)) {
    NS_WARNING("Selection start container couldn't have text nodes");
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  if (aEditSubAction == EditSubAction::eInsertTextComingFromIME) {
    EditorRawDOMPoint compositionStartPoint = GetCompositionStartPoint();
    if (!compositionStartPoint.IsSet()) {
      compositionStartPoint = pointToInsert;
    }

    if (aInsertionString.IsEmpty()) {
      // Right now the WhiteSpaceVisibilityKeeper code bails on empty strings,
      // but IME needs the InsertTextWithTransaction() call to still happen
      // since empty strings are meaningful there.
      nsresult rv = InsertTextWithTransaction(*document, aInsertionString,
                                              compositionStartPoint);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::InsertTextWithTransaction() failed");
      return EditActionHandled(rv);
    }

    EditorRawDOMPoint compositionEndPoint = GetCompositionEndPoint();
    if (!compositionEndPoint.IsSet()) {
      compositionEndPoint = compositionStartPoint;
    }
    nsresult rv = WhiteSpaceVisibilityKeeper::ReplaceText(
        *this, aInsertionString,
        EditorDOMRange(compositionStartPoint, compositionEndPoint));
    if (NS_FAILED(rv)) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::ReplaceText() failed");
      return EditActionHandled(rv);
    }

    compositionStartPoint = GetCompositionStartPoint();
    compositionEndPoint = GetCompositionEndPoint();
    if (NS_WARN_IF(!compositionStartPoint.IsSet()) ||
        NS_WARN_IF(!compositionEndPoint.IsSet())) {
      // Mutation event listener has changed the DOM tree...
      return EditActionHandled();
    }
    rv = TopLevelEditSubActionDataRef().mChangedRange->SetStartAndEnd(
        compositionStartPoint.ToRawRangeBoundary(),
        compositionEndPoint.ToRawRangeBoundary());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::SetStartAndEnd() failed");
    return EditActionHandled(rv);
  }

  MOZ_ASSERT(aEditSubAction == EditSubAction::eInsertText);

  // find where we are
  EditorDOMPoint currentPoint(pointToInsert);

  // is our text going to be PREformatted?
  // We remember this so that we know how to handle tabs.
  bool isPRE =
      EditorUtils::IsContentPreformatted(*pointToInsert.ContainerAsContent());

  // turn off the edit listener: we know how to
  // build the "doc changed range" ourselves, and it's
  // must faster to do it once here than to track all
  // the changes one at a time.
  AutoRestore<bool> disableListener(
      EditSubActionDataRef().mAdjustChangedRangeFromListener);
  EditSubActionDataRef().mAdjustChangedRangeFromListener = false;

  // don't change my selection in subtransactions
  AutoTransactionsConserveSelection dontChangeMySelection(*this);
  int32_t pos = 0;
  constexpr auto newlineStr = NS_LITERAL_STRING_FROM_CSTRING(LFSTR);

  {
    AutoTrackDOMPoint tracker(RangeUpdaterRef(), &pointToInsert);

    // for efficiency, break out the pre case separately.  This is because
    // its a lot cheaper to search the input string for only newlines than
    // it is to search for both tabs and newlines.
    if (isPRE || IsPlaintextEditor()) {
      while (pos != -1 &&
             pos < static_cast<int32_t>(aInsertionString.Length())) {
        int32_t oldPos = pos;
        int32_t subStrLen;
        pos = aInsertionString.FindChar(nsCRT::LF, oldPos);

        if (pos != -1) {
          subStrLen = pos - oldPos;
          // if first char is newline, then use just it
          if (!subStrLen) {
            subStrLen = 1;
          }
        } else {
          subStrLen = aInsertionString.Length() - oldPos;
          pos = aInsertionString.Length();
        }

        nsDependentSubstring subStr(aInsertionString, oldPos, subStrLen);

        // is it a return?
        if (subStr.Equals(newlineStr)) {
          RefPtr<Element> brElement =
              InsertBRElementWithTransaction(currentPoint, nsIEditor::eNone);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
          }
          if (!brElement) {
            NS_WARNING(
                "HTMLEditor::InsertBRElementWithTransaction(eNone) failed");
            return EditActionHandled(NS_ERROR_FAILURE);
          }
          pos++;
          if (brElement->GetNextSibling()) {
            pointToInsert.Set(brElement->GetNextSibling());
          } else {
            pointToInsert.SetToEndOf(currentPoint.GetContainer());
          }
          // XXX In most cases, pointToInsert and currentPoint are same here.
          //     But if the <br> element has been moved to different point by
          //     mutation observer, those points become different.
          currentPoint.SetAfter(brElement);
          NS_WARNING_ASSERTION(currentPoint.IsSet(),
                               "Failed to set after the <br> element");
          NS_WARNING_ASSERTION(currentPoint == pointToInsert,
                               "Perhaps, <br> element position has been moved "
                               "to different point "
                               "by mutation observer");
        } else {
          EditorRawDOMPoint pointAfterInsertedString;
          nsresult rv = InsertTextWithTransaction(
              *document, subStr, EditorRawDOMPoint(currentPoint),
              &pointAfterInsertedString);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
          }
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::InsertTextWithTransaction() failed");
            return EditActionHandled(rv);
          }
          currentPoint = pointAfterInsertedString;
          pointToInsert = pointAfterInsertedString;
        }
      }
    } else {
      constexpr auto tabStr = u"\t"_ns;
      constexpr auto spacesStr = u"    "_ns;
      char specialChars[] = {TAB, nsCRT::LF, 0};
      nsAutoString insertionString(aInsertionString);  // For FindCharInSet().
      while (pos != -1 &&
             pos < static_cast<int32_t>(insertionString.Length())) {
        int32_t oldPos = pos;
        int32_t subStrLen;
        pos = insertionString.FindCharInSet(specialChars, oldPos);

        if (pos != -1) {
          subStrLen = pos - oldPos;
          // if first char is newline, then use just it
          if (!subStrLen) {
            subStrLen = 1;
          }
        } else {
          subStrLen = insertionString.Length() - oldPos;
          pos = insertionString.Length();
        }

        nsDependentSubstring subStr(insertionString, oldPos, subStrLen);

        // is it a tab?
        if (subStr.Equals(tabStr)) {
          EditorRawDOMPoint pointAfterInsertedSpaces;
          nsresult rv = WhiteSpaceVisibilityKeeper::InsertText(
              *this, spacesStr, currentPoint, &pointAfterInsertedSpaces);
          if (NS_FAILED(rv)) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertText() failed");
            return EditActionHandled(rv);
          }
          pos++;
          MOZ_ASSERT(pointAfterInsertedSpaces.IsSet());
          currentPoint = pointAfterInsertedSpaces;
          pointToInsert = pointAfterInsertedSpaces;
        }
        // is it a return?
        else if (subStr.Equals(newlineStr)) {
          Result<RefPtr<Element>, nsresult> result =
              WhiteSpaceVisibilityKeeper::InsertBRElement(*this, currentPoint);
          if (result.isErr()) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertBRElement() failed");
            return EditActionHandled(result.inspectErr());
          }
          pos++;
          RefPtr<Element> newBRElement = result.unwrap();
          MOZ_DIAGNOSTIC_ASSERT(newBRElement);
          if (newBRElement->GetNextSibling()) {
            pointToInsert.Set(newBRElement->GetNextSibling());
          } else {
            pointToInsert.SetToEndOf(currentPoint.GetContainer());
          }
          currentPoint.SetAfter(newBRElement);
          NS_WARNING_ASSERTION(currentPoint.IsSet(),
                               "Failed to set after the new <br> element");
          // XXX If the newBRElement has been moved or removed by mutation
          //     observer, we hit this assert.  We need to check if
          //     newBRElement is in expected point, though, we must have
          //     a lot of same bugs...
          NS_WARNING_ASSERTION(
              currentPoint == pointToInsert,
              "Perhaps, newBRElement has been moved or removed unexpectedly");
        } else {
          EditorRawDOMPoint pointAfterInsertedString;
          nsresult rv = WhiteSpaceVisibilityKeeper::InsertText(
              *this, subStr, currentPoint, &pointAfterInsertedString);
          if (NS_FAILED(rv)) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertText() failed");
            return EditActionHandled(rv);
          }
          MOZ_ASSERT(pointAfterInsertedString.IsSet());
          currentPoint = pointAfterInsertedString;
          pointToInsert = pointAfterInsertedString;
        }
      }
    }

    // After this block, pointToInsert is updated by AutoTrackDOMPoint.
  }

  IgnoredErrorResult ignoredError;
  SelectionRefPtr()->SetInterlinePosition(false, ignoredError);
  NS_WARNING_ASSERTION(!ignoredError.Failed(),
                       "Failed to unset interline position");

  if (currentPoint.IsSet()) {
    nsresult rv = CollapseSelectionTo(currentPoint);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "Selection::Collapse() failed, but ignored");
  }

  // manually update the doc changed range so that AfterEdit will clean up
  // the correct portion of the document.
  if (currentPoint.IsSet()) {
    nsresult rv = TopLevelEditSubActionDataRef().mChangedRange->SetStartAndEnd(
        pointToInsert.ToRawRangeBoundary(), currentPoint.ToRawRangeBoundary());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::SetStartAndEnd() failed");
    return EditActionHandled(rv);
  }

  rv = TopLevelEditSubActionDataRef().mChangedRange->CollapseTo(pointToInsert);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::CollapseTo() failed");
  return EditActionHandled(rv);
}

EditActionResult HTMLEditor::InsertParagraphSeparatorAsSubAction() {
  if (NS_WARN_IF(!mInitSucceeded)) {
    return EditActionIgnored(NS_ERROR_NOT_INITIALIZED);
  }

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  // XXX This may be called by execCommand() with "insertParagraph".
  //     In such case, naming the transaction "TypingTxnName" is odd.
  AutoPlaceholderBatch treatAsOneTransaction(*this, *nsGkAtoms::TypingTxnName,
                                             ScrollSelectionIntoView::Yes);

  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eInsertParagraphSeparator, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  UndefineCaretBidiLevel();

  // If the selection isn't collapsed, delete it.
  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv =
        DeleteSelectionAsSubAction(nsIEditor::eNone, nsIEditor::eStrip);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::DeleteSelectionAsSubAction(eNone, eStrip) failed");
      return EditActionIgnored(rv);
    }
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  // Split any mailcites in the way.  Should we abort this if we encounter
  // table cell boundaries?
  if (IsMailEditor()) {
    EditorDOMPoint pointToSplit(EditorBase::GetStartPoint(*SelectionRefPtr()));
    if (NS_WARN_IF(!pointToSplit.IsSet())) {
      return EditActionIgnored(NS_ERROR_FAILURE);
    }

    EditActionResult result = SplitMailCiteElements(pointToSplit);
    if (result.Failed()) {
      NS_WARNING("HTMLEditor::SplitMailCiteElements() failed");
      return result;
    }
    if (result.Handled()) {
      return result;
    }
  }

  // Smart splitting rules
  const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return EditActionIgnored(NS_ERROR_FAILURE);
  }

  EditorDOMPoint atStartOfSelection(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return EditActionIgnored(NS_ERROR_FAILURE);
  }
  MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

  // Do nothing if the node is read-only
  if (!HTMLEditUtils::IsSimplyEditableNode(
          *atStartOfSelection.GetContainer())) {
    return EditActionCanceled();
  }

  // If the active editing host is an inline element, or if the active editing
  // host is the block parent itself and we're configured to use <br> as a
  // paragraph separator, just append a <br>.
  RefPtr<Element> editingHost = GetActiveEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return EditActionIgnored(NS_ERROR_FAILURE);
  }

  // Look for the nearest parent block.  However, don't return error even if
  // there is no block parent here because in such case, i.e., editing host
  // is an inline element, we should insert <br> simply.
  RefPtr<Element> blockElement =
      atStartOfSelection.IsInContentNode()
          ? HTMLEditUtils::GetInclusiveAncestorBlockElement(
                *atStartOfSelection.ContainerAsContent(), editingHost)
          : nullptr;

  ParagraphSeparator separator = GetDefaultParagraphSeparator();
  bool insertBRElement;
  // If there is no block parent in the editing host, i.e., the editing host
  // itself is also a non-block element, we should insert a <br> element.
  if (!blockElement) {
    // XXX Chromium checks if the CSS box of the editing host is a block.
    insertBRElement = true;
  }
  // If only the editing host is block, and the default paragraph separator
  // is <br> or the editing host cannot contain a <p> element, we should
  // insert a <br> element.
  else if (editingHost == blockElement) {
    insertBRElement = separator == ParagraphSeparator::br ||
                      !HTMLEditUtils::CanElementContainParagraph(*editingHost);
  }
  // If the nearest block parent is a single-line container declared in
  // the execCommand spec and not the editing host, we should separate the
  // block even if the default paragraph separator is <br> element.
  else if (HTMLEditUtils::IsSingleLineContainer(*blockElement)) {
    insertBRElement = false;
  }
  // Otherwise, unless there is no block ancestor which can contain <p>
  // element, we shouldn't insert a <br> element here.
  else {
    insertBRElement = true;
    for (Element* blockAncestor = blockElement;
         blockAncestor && insertBRElement;
         blockAncestor = HTMLEditUtils::GetAncestorBlockElement(*blockAncestor,
                                                                editingHost)) {
      insertBRElement =
          !HTMLEditUtils::CanElementContainParagraph(*blockAncestor);
    }
  }

  // If we cannot insert a <p>/<div> element at the selection, we should insert
  // a <br> element instead.
  if (insertBRElement) {
    nsresult rv = InsertBRElement(atStartOfSelection);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::InsertBRElement() failed");
      return EditActionIgnored(rv);
    }
    return EditActionHandled();
  }

  if (editingHost == blockElement && separator != ParagraphSeparator::br) {
    // Insert a new block first
    MOZ_ASSERT(separator == ParagraphSeparator::div ||
               separator == ParagraphSeparator::p);
    // FormatBlockContainerWithTransaction() creates AutoSelectionRestorer.
    // Therefore, even if it returns NS_OK, editor might have been destroyed
    // at restoring Selection.
    nsresult rv = FormatBlockContainerWithTransaction(
        MOZ_KnownLive(HTMLEditor::ToParagraphSeparatorTagName(separator)));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED) ||
        NS_WARN_IF(Destroyed())) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    // We warn on failure, but don't handle it, because it might be harmless.
    // Instead we just check that a new block was actually created.
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::FormatBlockContainerWithTransaction() "
                         "failed, but ignored");

    firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionIgnored(NS_ERROR_FAILURE);
    }

    atStartOfSelection = firstRange->StartRef();
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionIgnored(NS_ERROR_FAILURE);
    }
    MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

    blockElement =
        atStartOfSelection.IsInContentNode()
            ? HTMLEditUtils::GetInclusiveAncestorBlockElement(
                  *atStartOfSelection.ContainerAsContent(), editingHost)
            : nullptr;
    if (NS_WARN_IF(!blockElement)) {
      return EditActionIgnored(NS_ERROR_UNEXPECTED);
    }
    if (NS_WARN_IF(blockElement == editingHost)) {
      // Didn't create a new block for some reason, fall back to <br>
      rv = InsertBRElement(atStartOfSelection);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::InsertBRElement() failed");
        return EditActionIgnored(rv);
      }
      return EditActionHandled();
    }
    // Now, mNewBlockElement is last created block element for wrapping inline
    // elements around the caret position and AfterEditInner() will move
    // caret into it.  However, it may be different from block parent of
    // the caret position.  E.g., FormatBlockContainerWithTransaction() may
    // wrap following inline elements of a <br> element which is next sibling
    // of container of the caret.  So, we need to adjust mNewBlockElement here
    // for avoiding jumping caret to odd position.
    TopLevelEditSubActionDataRef().mNewBlockElement = blockElement;
  }

  // If block is empty, populate with br.  (For example, imagine a div that
  // contains the word "text".  The user selects "text" and types return.
  // "Text" is deleted leaving an empty block.  We want to put in one br to
  // make block have a line.  Then code further below will put in a second br.)
  if (IsEmptyBlockElement(*blockElement, IgnoreSingleBR::No)) {
    AutoEditorDOMPointChildInvalidator lockOffset(atStartOfSelection);
    EditorDOMPoint endOfBlockParent;
    endOfBlockParent.SetToEndOf(blockElement);
    RefPtr<Element> brElement =
        InsertBRElementWithTransaction(endOfBlockParent);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    if (!brElement) {
      NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
      return EditActionIgnored(NS_ERROR_FAILURE);
    }
  }

  RefPtr<Element> listItem = GetNearestAncestorListItemElement(*blockElement);
  if (listItem && listItem != editingHost) {
    nsresult rv = HandleInsertParagraphInListItemElement(
        *listItem, MOZ_KnownLive(*atStartOfSelection.GetContainer()),
        atStartOfSelection.Offset());
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::HandleInsertParagraphInListItemElement() "
                         "failed, but ignored");
    return EditActionHandled();
  }

  if (HTMLEditUtils::IsHeader(*blockElement)) {
    // Headers: close (or split) header
    nsresult rv = HandleInsertParagraphInHeadingElement(
        *blockElement, MOZ_KnownLive(*atStartOfSelection.GetContainer()),
        atStartOfSelection.Offset());
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::HandleInsertParagraphInHeadingElement() "
                         "failed, but ignored");
    return EditActionHandled();
  }

  // XXX Ideally, we should take same behavior with both <p> container and
  //     <div> container.  However, we are still using <br> as default
  //     paragraph separator (non-standard) and we've split only <p> container
  //     long time.  Therefore, some web apps may depend on this behavior like
  //     Gmail.  So, let's use traditional odd behavior only when the default
  //     paragraph separator is <br>.  Otherwise, take consistent behavior
  //     between <p> container and <div> container.
  if ((separator == ParagraphSeparator::br &&
       blockElement->IsHTMLElement(nsGkAtoms::p)) ||
      (separator != ParagraphSeparator::br &&
       blockElement->IsAnyOfHTMLElements(nsGkAtoms::p, nsGkAtoms::div))) {
    AutoEditorDOMPointChildInvalidator lockOffset(atStartOfSelection);
    // Paragraphs: special rules to look for <br>s
    EditActionResult result = HandleInsertParagraphInParagraph(*blockElement);
    if (result.Failed()) {
      NS_WARNING("HTMLEditor::HandleInsertParagraphInParagraph() failed");
      return result;
    }
    if (result.Handled()) {
      // Now, atStartOfSelection may be invalid because the left paragraph
      // may have less children than its offset.  For avoiding warnings of
      // validation of EditorDOMPoint, we should not touch it anymore.
      lockOffset.Cancel();
      return result;
    }
    // Fall through, if HandleInsertParagraphInParagraph() didn't handle it.
    MOZ_ASSERT(!result.Canceled(),
               "HandleInsertParagraphInParagraph() canceled this edit action, "
               "InsertParagraphSeparatorAsSubAction() needs to handle this "
               "action instead");
  }

  // If nobody handles this edit action, let's insert new <br> at the selection.
  rv = InsertBRElement(atStartOfSelection);
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::InsertBRElement() failed");
    return EditActionIgnored(rv);
  }
  return EditActionHandled();
}

nsresult HTMLEditor::InsertBRElement(const EditorDOMPoint& aPointToBreak) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(!aPointToBreak.IsSet())) {
    return NS_ERROR_INVALID_ARG;
  }

  bool brElementIsAfterBlock = false, brElementIsBeforeBlock = false;

  // First, insert a <br> element.
  RefPtr<Element> brElement;
  if (IsPlaintextEditor()) {
    brElement = InsertBRElementWithTransaction(aPointToBreak);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!brElement) {
      NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }
  } else {
    EditorDOMPoint pointToBreak(aPointToBreak);
    WSRunScanner wsRunScanner(*this, pointToBreak);
    brElementIsAfterBlock =
        wsRunScanner.ScanPreviousVisibleNodeOrBlockBoundaryFrom(pointToBreak)
            .ReachedBlockBoundary();
    brElementIsBeforeBlock =
        wsRunScanner.ScanNextVisibleNodeOrBlockBoundaryFrom(pointToBreak)
            .ReachedBlockBoundary();
    // If the container of the break is a link, we need to split it and
    // insert new <br> between the split links.
    RefPtr<Element> linkNode =
        HTMLEditor::GetLinkElement(pointToBreak.GetContainer());
    if (linkNode) {
      SplitNodeResult splitLinkNodeResult = SplitNodeDeepWithTransaction(
          *linkNode, pointToBreak, SplitAtEdges::eDoNotCreateEmptyContainer);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (splitLinkNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) failed");
        return splitLinkNodeResult.Rv();
      }
      pointToBreak = splitLinkNodeResult.SplitPoint();
    }
    Result<RefPtr<Element>, nsresult> result =
        WhiteSpaceVisibilityKeeper::InsertBRElement(*this, pointToBreak);
    if (result.isErr()) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::InsertBRElement() failed");
      return result.inspectErr();
    }
    brElement = result.unwrap();
    MOZ_ASSERT(brElement);
  }

  // If the <br> element has already been removed from the DOM tree by a
  // mutation event listener, don't continue handling this.
  if (NS_WARN_IF(!brElement->GetParentNode())) {
    return NS_ERROR_FAILURE;
  }

  if (brElementIsAfterBlock && brElementIsBeforeBlock) {
    // We just placed a <br> between block boundaries.  This is the one case
    // where we want the selection to be before the br we just placed, as the
    // br will be on a new line, rather than at end of prior line.
    // XXX brElementIsAfterBlock and brElementIsBeforeBlock were set before
    //     modifying the DOM tree.  So, now, the <br> element may not be
    //     between blocks.
    IgnoredErrorResult ignoredError;
    SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
    NS_WARNING_ASSERTION(
        !ignoredError.Failed(),
        "Selection::SetInterlinePosition(true) failed, but ignored");
    nsresult rv = CollapseSelectionTo(EditorRawDOMPoint(brElement));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionTo() failed");
    return rv;
  }

  EditorDOMPoint afterBRElement(brElement);
  DebugOnly<bool> advanced = afterBRElement.AdvanceOffset();
  NS_WARNING_ASSERTION(advanced,
                       "Failed to advance offset after the new <br> element");
  WSScanResult forwardScanFromAfterBRElementResult =
      WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(*this, afterBRElement);
  if (forwardScanFromAfterBRElementResult.ReachedBRElement()) {
    // The next thing after the break we inserted is another break.  Move the
    // second break to be the first break's sibling.  This will prevent them
    // from being in different inline nodes, which would break
    // SetInterlinePosition().  It will also assure that if the user clicks
    // away and then clicks back on their new blank line, they will still get
    // the style from the line above.
    if (brElement->GetNextSibling() !=
        forwardScanFromAfterBRElementResult.BRElementPtr()) {
      MOZ_ASSERT(forwardScanFromAfterBRElementResult.BRElementPtr());
      nsresult rv = MoveNodeWithTransaction(
          MOZ_KnownLive(*forwardScanFromAfterBRElementResult.BRElementPtr()),
          afterBRElement);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
        return rv;
      }
    }
  }

  // SetInterlinePosition(true) means we want the caret to stick to the
  // content on the "right".  We want the caret to stick to whatever is past
  // the break.  This is because the break is on the same line we were on,
  // but the next content will be on the following line.

  // An exception to this is if the break has a next sibling that is a block
  // node.  Then we stick to the left to avoid an uber caret.
  nsIContent* nextSiblingOfBRElement = brElement->GetNextSibling();
  IgnoredErrorResult ignoredError;
  SelectionRefPtr()->SetInterlinePosition(
      !(nextSiblingOfBRElement &&
        HTMLEditUtils::IsBlockElement(*nextSiblingOfBRElement)),
      ignoredError);
  NS_WARNING_ASSERTION(!ignoredError.Failed(),
                       "Selection::SetInterlinePosition() failed, but ignored");
  nsresult rv = CollapseSelectionTo(afterBRElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

EditActionResult HTMLEditor::SplitMailCiteElements(
    const EditorDOMPoint& aPointToSplit) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToSplit.IsSet());

  RefPtr<Element> citeNode =
      GetMostAncestorMailCiteElement(*aPointToSplit.GetContainer());
  if (!citeNode) {
    return EditActionIgnored();
  }

  EditorDOMPoint pointToSplit(aPointToSplit);

  // If our selection is just before a break, nudge it to be just after it.
  // This does two things for us.  It saves us the trouble of having to add
  // a break here ourselves to preserve the "blockness" of the inline span
  // mailquote (in the inline case), and :
  // it means the break won't end up making an empty line that happens to be
  // inside a mailquote (in either inline or block case).
  // The latter can confuse a user if they click there and start typing,
  // because being in the mailquote may affect wrapping behavior, or font
  // color, etc.
  WSScanResult forwardScanFromPointToSplitResult =
      WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(*this, pointToSplit);
  // If selection start point is before a break and it's inside the mailquote,
  // let's split it after the visible node.
  if (forwardScanFromPointToSplitResult.ReachedBRElement() &&
      forwardScanFromPointToSplitResult.BRElementPtr() != citeNode &&
      citeNode->Contains(forwardScanFromPointToSplitResult.BRElementPtr())) {
    pointToSplit = forwardScanFromPointToSplitResult.PointAfterContent();
  }

  if (NS_WARN_IF(!pointToSplit.GetContainerAsContent())) {
    return EditActionIgnored(NS_ERROR_FAILURE);
  }

  SplitNodeResult splitCiteNodeResult = SplitNodeDeepWithTransaction(
      *citeNode, pointToSplit, SplitAtEdges::eDoNotCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
  }
  if (splitCiteNodeResult.Failed()) {
    NS_WARNING(
        "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
        "eDoNotCreateEmptyContainer) failed");
    return EditActionIgnored(splitCiteNodeResult.Rv());
  }
  pointToSplit.Clear();

  // Add an invisible <br> to the end of current cite node (If new left cite
  // has not been created, we're at the end of it.  Otherwise, we're still at
  // the right node) if it was a <span> of style="display: block". This is
  // important, since when serializing the cite to plain text, the span which
  // caused the visual break is discarded.  So the added <br> will guarantee
  // that the serializer will insert a break where the user saw one.
  // FYI: splitCiteNodeResult grabs the previous node with nsCOMPtr.  So, it's
  //      safe to access previousNodeOfSplitPoint even after changing the DOM
  //      tree and/or selection even though it's raw pointer.
  nsIContent* previousNodeOfSplitPoint = splitCiteNodeResult.GetPreviousNode();
  if (previousNodeOfSplitPoint &&
      previousNodeOfSplitPoint->IsHTMLElement(nsGkAtoms::span) &&
      previousNodeOfSplitPoint->GetPrimaryFrame() &&
      previousNodeOfSplitPoint->GetPrimaryFrame()->IsBlockFrameOrSubclass()) {
    nsCOMPtr<nsINode> lastChild = previousNodeOfSplitPoint->GetLastChild();
    if (lastChild && !lastChild->IsHTMLElement(nsGkAtoms::br)) {
      // We ignore the result here.
      EditorDOMPoint endOfPreviousNodeOfSplitPoint;
      endOfPreviousNodeOfSplitPoint.SetToEndOf(previousNodeOfSplitPoint);
      RefPtr<Element> invisibleBRElement =
          InsertBRElementWithTransaction(endOfPreviousNodeOfSplitPoint);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          invisibleBRElement,
          "HTMLEditor::InsertBRElementWithTransaction() failed, but ignored");
    }
  }

  // In most cases, <br> should be inserted after current cite.  However, if
  // left cite hasn't been created because the split point was start of the
  // cite node, <br> should be inserted before the current cite.
  EditorDOMPoint pointToInsertBRNode(splitCiteNodeResult.SplitPoint());
  RefPtr<Element> brElement =
      InsertBRElementWithTransaction(pointToInsertBRNode);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
  }
  if (!brElement) {
    NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
    return EditActionIgnored(NS_ERROR_FAILURE);
  }
  // Now, offset of pointToInsertBRNode is invalid.  Let's clear it.
  pointToInsertBRNode.Clear();

  // Want selection before the break, and on same line.
  EditorDOMPoint atBRElement(brElement);
  {
    AutoEditorDOMPointChildInvalidator lockOffset(atBRElement);
    IgnoredErrorResult ignoredError;
    SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
    NS_WARNING_ASSERTION(
        !ignoredError.Failed(),
        "Selection::SetInterlinePosition(true) failed, but ignored");
    nsresult rv = CollapseSelectionTo(atBRElement);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::CollapseSelectionTo() failed");
      return EditActionIgnored(rv);
    }
  }

  // if citeNode wasn't a block, we might also want another break before it.
  // We need to examine the content both before the br we just added and also
  // just after it.  If we don't have another br or block boundary adjacent,
  // then we will need a 2nd br added to achieve blank line that user expects.
  if (HTMLEditUtils::IsInlineElement(*citeNode)) {
    // Use DOM point which we tried to collapse to.
    EditorDOMPoint pointToCreateNewBRElement(atBRElement);

    WSScanResult backwardScanFromPointToCreateNewBRElementResult =
        WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundary(
            *this, pointToCreateNewBRElement);
    if (backwardScanFromPointToCreateNewBRElementResult
            .InNormalWhiteSpacesOrText() ||
        backwardScanFromPointToCreateNewBRElementResult
            .ReachedSpecialContent()) {
      EditorRawDOMPoint pointAfterNewBRElement(
          EditorRawDOMPoint::After(pointToCreateNewBRElement));
      NS_WARNING_ASSERTION(pointAfterNewBRElement.IsSet(),
                           "Failed to set to after the <br> node");
      WSScanResult forwardScanFromPointAfterNewBRElementResult =
          WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
              *this, pointAfterNewBRElement);
      if (forwardScanFromPointAfterNewBRElementResult
              .InNormalWhiteSpacesOrText() ||
          forwardScanFromPointAfterNewBRElementResult.ReachedSpecialContent() ||
          // In case we're at the very end.
          forwardScanFromPointAfterNewBRElementResult
              .ReachedCurrentBlockBoundary()) {
        brElement = InsertBRElementWithTransaction(pointToCreateNewBRElement);
        if (NS_WARN_IF(Destroyed())) {
          return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
        }
        if (!brElement) {
          NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
          return EditActionIgnored(NS_ERROR_FAILURE);
        }
        // Now, those points may be invalid.
        pointToCreateNewBRElement.Clear();
        pointAfterNewBRElement.Clear();
      }
    }
  }

  // delete any empty cites
  if (previousNodeOfSplitPoint &&
      IsEmptyNode(*previousNodeOfSplitPoint, true, false)) {
    nsresult rv =
        DeleteNodeWithTransaction(MOZ_KnownLive(*previousNodeOfSplitPoint));
    if (NS_WARN_IF(Destroyed())) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return EditActionIgnored(rv);
    }
  }

  if (citeNode && IsEmptyNode(*citeNode, true, false)) {
    nsresult rv = DeleteNodeWithTransaction(*citeNode);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return EditActionIgnored(rv);
    }
  }

  return EditActionHandled();
}

/**
 * AutoDeleteRangesHandler class handles deleting ranges in HTMLEditor.
 */
class MOZ_STACK_CLASS HTMLEditor::AutoDeleteRangesHandler final {
 public:
  explicit AutoDeleteRangesHandler(
      const AutoDeleteRangesHandler* aParent = nullptr)
      : mParent(aParent),
        mOriginalDirectionAndAmount(nsIEditor::eNone),
        mOriginalStripWrappers(nsIEditor::eNoStrip) {}

  /**
   * ComputeRangesToDelete() computes actual deletion ranges.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult ComputeRangesToDelete(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete);

  /**
   * Deletes content in or around aRangesToDelete.
   * NOTE: This method creates SelectionBatcher.  Therefore, each caller
   *       needs to check if the editor is still available even if this returns
   *       NS_OK.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  Run(HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      nsIEditor::EStripWrappers aStripWrappers,
      AutoRangeArray& aRangesToDelete);

 private:
  bool IsHandlingRecursively() const { return mParent != nullptr; }

  bool CanFallbackToDeleteRangesWithTransaction(
      const AutoRangeArray& aRangesToDelete) const {
    return !IsHandlingRecursively() && !aRangesToDelete.Ranges().IsEmpty() &&
           (!aRangesToDelete.IsCollapsed() ||
            EditorBase::HowToHandleCollapsedRangeFor(
                mOriginalDirectionAndAmount) !=
                EditorBase::HowToHandleCollapsedRange::Ignore);
  }

  /**
   * HandleDeleteAroundCollapsedRanges() handles deletion with collapsed
   * ranges.  Callers must guarantee that this is called only when
   * aRangesToDelete.IsCollapsed() returns true.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aStripWrappers      Must be eStrip or eNoStrip.
   * @param aRangesToDelete     Ranges to delete.  This `IsCollapsed()` must
   *                            return true.
   * @param aWSRunScannerAtCaret        Scanner instance which scanned from
   *                                    caret point.
   * @param aScanFromCaretPointResult   Scan result of aWSRunScannerAtCaret
   *                                    toward aDirectionAndAmount.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteAroundCollapsedRanges(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
      const WSRunScanner& aWSRunScannerAtCaret,
      const WSScanResult& aScanFromCaretPointResult);
  nsresult ComputeRangesToDeleteAroundCollapsedRanges(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete, const WSRunScanner& aWSRunScannerAtCaret,
      const WSScanResult& aScanFromCaretPointResult) const;

  /**
   * HandleDeleteNonCollapsedRanges() handles deletion with non-collapsed
   * ranges.  Callers must guarantee that this is called only when
   * aRangesToDelete.IsCollapsed() returns false.
   *
   * @param aDirectionAndAmount         Direction of the deletion.
   * @param aStripWrappers              Must be eStrip or eNoStrip.
   * @param aRangesToDelete             The ranges to delete.
   * @param aSelectionWasCollapsed      If the caller extended `Selection`
   *                                    from collapsed, set this to `Yes`.
   *                                    Otherwise, i.e., `Selection` is not
   *                                    collapsed from the beginning, set
   *                                    this to `No`.
   */
  enum class SelectionWasCollapsed { Yes, No };
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteNonCollapsedRanges(HTMLEditor& aHTMLEditor,
                                 nsIEditor::EDirection aDirectionAndAmount,
                                 nsIEditor::EStripWrappers aStripWrappers,
                                 AutoRangeArray& aRangesToDelete,
                                 SelectionWasCollapsed aSelectionWasCollapsed);
  nsresult ComputeRangesToDeleteNonCollapsedRanges(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete,
      SelectionWasCollapsed aSelectionWasCollapsed) const;

  /**
   * HandleDeleteTextAroundCollapsedRanges() handles deletion of collapsed
   * ranges in a text node.
   *
   * @param aDirectionAndAmount Must be eNext or ePrevious.
   * @param aCaretPoisition     The position where caret is.  This container
   *                            must be a text node.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteTextAroundCollapsedRanges(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete);
  nsresult ComputeRangesToDeleteTextAroundCollapsedRanges(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * HandleDeleteCollapsedSelectionAtWhiteSpaces() handles deletion of
   * collapsed selection at white-spaces in a text node.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aPointToDelete      The point to delete.  I.e., typically, caret
   *                            position.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteCollapsedSelectionAtWhiteSpaces(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aPointToDelete);

  /**
   * HandleDeleteCollapsedSelectionAtVisibleChar() handles deletion of
   * collapsed selection in a text node.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aPointToDelete      The point in a text node to delete character(s).
   *                            Caller must guarantee that this is in a text
   *                            node.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteCollapsedSelectionAtVisibleChar(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aPointToDelete);

  /**
   * HandleDeleteAtomicContent() handles deletion of atomic elements like
   * `<br>`, `<hr>`, `<img>`, `<input>`, etc and data nodes except text node
   * (e.g., comment node). Note that don't call this directly with `<hr>`
   * element.  Instead, call `HandleDeleteHRElement()`. Note that don't call
   * this for invisible `<br>` element.
   *
   * @param aAtomicContent      The atomic content to be deleted.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteAtomicContent(HTMLEditor& aHTMLEditor, nsIContent& aAtomicContent,
                            const EditorDOMPoint& aCaretPoint,
                            const WSRunScanner& aWSRunScannerAtCaret);
  nsresult ComputeRangesToDeleteAtomicContent(
      const HTMLEditor& aHTMLEditor, const nsIContent& aAtomicContent,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * HandleDeleteHRElement() handles deletion around `<hr>` element.  If
   * aDirectionAndAmount is nsIEditor::ePrevious, aHTElement is removed only
   * when caret is at next sibling of the `<hr>` element and inter line position
   * is "left".  Otherwise, caret is moved and does not remove the `<hr>`
   * elemnent.
   * XXX Perhaps, we can get rid of this special handling because the other
   *     browsers don't do this, and our `<hr>` element handling is really
   *     odd.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aHRElement          The `<hr>` element to be removed.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult HandleDeleteHRElement(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      Element& aHRElement, const EditorDOMPoint& aCaretPoint,
      const WSRunScanner& aWSRunScannerAtCaret);
  nsresult ComputeRangesToDeleteHRElement(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      Element& aHRElement, const EditorDOMPoint& aCaretPoint,
      const WSRunScanner& aWSRunScannerAtCaret,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * HandleDeleteAtOtherBlockBoundary() handles deletion at other block boundary
   * (i.e., immediately before or after a block). If this does not join blocks,
   * `Run()` may be called recursively with creating another instance.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aStripWrappers      Must be eStrip or eNoStrip.
   * @param aOtherBlockElement  The block element which follows the caret or
   *                            is followed by caret.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   * @param aRangesToDelete     Ranges to delete of the caller.  This should
   *                            be collapsed and the point should match with
   *                            aCaretPoint.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  HandleDeleteAtOtherBlockBoundary(HTMLEditor& aHTMLEditor,
                                   nsIEditor::EDirection aDirectionAndAmount,
                                   nsIEditor::EStripWrappers aStripWrappers,
                                   Element& aOtherBlockElement,
                                   const EditorDOMPoint& aCaretPoint,
                                   WSRunScanner& aWSRunScannerAtCaret,
                                   AutoRangeArray& aRangesToDelete);

  /**
   * ExtendRangeToIncludeInvisibleNodes() extends aRange if there are some
   * invisible nodes around it.
   *
   * @param aFrameSelection     If the caller wants range in selection limiter,
   *                            set this to non-nullptr which knows the limiter.
   * @param aRange              The range to be extended.  This must not be
   *                            collapsed, must be positioned, and must not be
   *                            in selection.
   * @return                    true if succeeded to set the range.
   */
  bool ExtendRangeToIncludeInvisibleNodes(
      const HTMLEditor& aHTMLEditor, const nsFrameSelection* aFrameSelection,
      nsRange& aRange) const;

  /**
   * ShouldDeleteHRElement() checks whether aHRElement should be deleted
   * when selection is collapsed at aCaretPoint.
   */
  Result<bool, nsresult> ShouldDeleteHRElement(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      Element& aHRElement, const EditorDOMPoint& aCaretPoint) const;

  /**
   * DeleteUnnecessaryNodesAndCollapseSelection() removes unnecessary nodes
   * around aSelectionStartPoint and aSelectionEndPoint.  Then, collapse
   * selection at aSelectionStartPoint or aSelectionEndPoint (depending on
   * aDirectionAndAmount).
   *
   * @param aDirectionAndAmount         Direction of the deletion.
   *                                    If nsIEditor::ePrevious, selection
   * will be collapsed to aSelectionEndPoint. Otherwise, selection will be
   * collapsed to aSelectionStartPoint.
   * @param aSelectionStartPoint        First selection range start after
   *                                    computing the deleting range.
   * @param aSelectionEndPoint          First selection range end after
   *                                    computing the deleting range.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteUnnecessaryNodesAndCollapseSelection(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aSelectionStartPoint,
      const EditorDOMPoint& aSelectionEndPoint);

  /**
   * If aContent is a text node that contains only collapsed white-space or
   * empty and editable.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteNodeIfInvisibleAndEditableTextNode(HTMLEditor& aHTMLEditor,
                                           nsIContent& aContent);

  /**
   * DeleteParentBlocksIfEmpty() removes parent block elements if they
   * don't have visible contents.  Note that due performance issue of
   * WhiteSpaceVisibilityKeeper, this call may be expensive.  And also note that
   * this removes a empty block with a transaction.  So, please make sure that
   * you've already created `AutoPlaceholderBatch`.
   *
   * @param aPoint      The point whether this method climbing up the DOM
   *                    tree to remove empty parent blocks.
   * @return            NS_OK if one or more empty block parents are deleted.
   *                    NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND if the point is
   *                    not in empty block.
   *                    Or NS_ERROR_* if something unexpected occurs.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteParentBlocksWithTransactionIfEmpty(HTMLEditor& aHTMLEditor,
                                           const EditorDOMPoint& aPoint);

  [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
  FallbackToDeleteRangesWithTransaction(HTMLEditor& aHTMLEditor,
                                        AutoRangeArray& aRangesToDelete) {
    MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
    MOZ_ASSERT(CanFallbackToDeleteRangesWithTransaction(aRangesToDelete));
    nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
        mOriginalDirectionAndAmount, mOriginalStripWrappers, aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::DeleteRangesWithTransaction() failed");
    return EditActionHandled(rv);  // Don't return "ignored" for avoiding to
                                   // fall it back again.
  }

  /**
   * ComputeRangesToDeleteRangesWithTransaction() computes target ranges
   * which will be called by `EditorBase::DeleteRangesWithTransaction()`.
   * TODO: We should not use it for consistency with each deletion handler
   *       in this and nested classes.
   */
  nsresult ComputeRangesToDeleteRangesWithTransaction(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete) const;

  nsresult FallbackToComputeRangesToDeleteRangesWithTransaction(
      const HTMLEditor& aHTMLEditor, AutoRangeArray& aRangesToDelete) const {
    MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
    MOZ_ASSERT(CanFallbackToDeleteRangesWithTransaction(aRangesToDelete));
    nsresult rv = ComputeRangesToDeleteRangesWithTransaction(
        aHTMLEditor, mOriginalDirectionAndAmount, aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoDeleteRangesHandler::"
                         "ComputeRangesToDeleteRangesWithTransaction() failed");
    return rv;
  }

  class MOZ_STACK_CLASS AutoBlockElementsJoiner final {
   public:
    AutoBlockElementsJoiner() = delete;
    explicit AutoBlockElementsJoiner(
        AutoDeleteRangesHandler& aDeleteRangesHandler)
        : mDeleteRangesHandler(&aDeleteRangesHandler),
          mDeleteRangesHandlerConst(aDeleteRangesHandler) {}
    explicit AutoBlockElementsJoiner(
        const AutoDeleteRangesHandler& aDeleteRangesHandler)
        : mDeleteRangesHandler(nullptr),
          mDeleteRangesHandlerConst(aDeleteRangesHandler) {}

    /**
     * PrepareToDeleteAtCurrentBlockBoundary() considers left content and right
     * content which are joined for handling deletion at current block boundary
     * (i.e., at start or end of the current block).
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aCurrentBlockElement      The current block element.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        Element& aCurrentBlockElement, const EditorDOMPoint& aCaretPoint);

    /**
     * PrepareToDeleteAtOtherBlockBoundary() considers left content and right
     * content which are joined for handling deletion at other block boundary
     * (i.e., immediately before or after a block).
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aOtherBlockElement        The block element which follows the
     *                                  caret or is followed by caret.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @param aWSRunScannerAtCaret      WSRunScanner instance which was
     *                                  initialized with the caret point.
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount, Element& aOtherBlockElement,
        const EditorDOMPoint& aCaretPoint,
        const WSRunScanner& aWSRunScannerAtCaret);

    /**
     * PrepareToDeleteNonCollapsedRanges() considers left block element and
     * right block element which are inclusive ancestor block element of
     * start and end container of first range of aRangesToDelete.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aRangesToDelete           Ranges to delete.  Must not be
     *                                  collapsed.
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor, const AutoRangeArray& aRangesToDelete);

    /**
     * Run() executes the joining.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aStripWrappers            Must be eStrip or eNoStrip.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @param aRangesToDelete           Ranges to delete of the caller.
     *                                  This should be collapsed and match
     *                                  with aCaretPoint.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    Run(HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        const EditorDOMPoint& aCaretPoint, AutoRangeArray& aRangesToDelete) {
      switch (mMode) {
        case Mode::JoinCurrentBlock: {
          EditActionResult result =
              HandleDeleteAtCurrentBlockBoundary(aHTMLEditor, aCaretPoint);
          NS_WARNING_ASSERTION(result.Succeeded(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteAtCurrentBlockBoundary() failed");
          return result;
        }
        case Mode::JoinOtherBlock: {
          EditActionResult result = HandleDeleteAtOtherBlockBoundary(
              aHTMLEditor, aDirectionAndAmount, aStripWrappers, aCaretPoint,
              aRangesToDelete);
          NS_WARNING_ASSERTION(result.Succeeded(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteAtOtherBlockBoundary() failed");
          return result;
        }
        case Mode::DeleteBRElement: {
          EditActionResult result =
              DeleteBRElement(aHTMLEditor, aDirectionAndAmount, aCaretPoint);
          NS_WARNING_ASSERTION(
              result.Succeeded(),
              "AutoBlockElementsJoiner::DeleteBRElement() failed");
          return result;
        }
        case Mode::JoinBlocksInSameParent:
        case Mode::DeleteContentInRanges:
        case Mode::DeleteNonCollapsedRanges:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other Run()");
          return EditActionResult(NS_ERROR_UNEXPECTED);
        case Mode::NotInitialized:
          return EditActionIgnored();
      }
      return EditActionResult(NS_ERROR_NOT_INITIALIZED);
    }

    nsresult ComputeRangesToDelete(const HTMLEditor& aHTMLEditor,
                                   nsIEditor::EDirection aDirectionAndAmount,
                                   const EditorDOMPoint& aCaretPoint,
                                   AutoRangeArray& aRangesToDelete) const {
      switch (mMode) {
        case Mode::JoinCurrentBlock: {
          nsresult rv = ComputeRangesToDeleteAtCurrentBlockBoundary(
              aHTMLEditor, aCaretPoint, aRangesToDelete);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteAtCurrentBlockBoundary() failed");
          return rv;
        }
        case Mode::JoinOtherBlock: {
          nsresult rv = ComputeRangesToDeleteAtOtherBlockBoundary(
              aHTMLEditor, aDirectionAndAmount, aCaretPoint, aRangesToDelete);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteAtOtherBlockBoundary() failed");
          return rv;
        }
        case Mode::DeleteBRElement: {
          nsresult rv = ComputeRangesToDeleteBRElement(aRangesToDelete);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "AutoBlockElementsJoiner::"
                               "ComputeRangesToDeleteBRElement() failed");
          return rv;
        }
        case Mode::JoinBlocksInSameParent:
        case Mode::DeleteContentInRanges:
        case Mode::DeleteNonCollapsedRanges:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other "
              "ComputeRangesToDelete()");
          return NS_ERROR_UNEXPECTED;
        case Mode::NotInitialized:
          return NS_OK;
      }
      return NS_ERROR_NOT_IMPLEMENTED;
    }

    /**
     * Run() executes the joining.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aStripWrappers            Whether delete or keep new empty
     *                                  ancestor elements.
     * @param aRangesToDelete           Ranges to delete.  Must not be
     *                                  collapsed.
     * @param aSelectionWasCollapsed    Whether selection was or was not
     *                                  collapsed when starting to handle
     *                                  deletion.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    Run(HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed) {
      switch (mMode) {
        case Mode::JoinCurrentBlock:
        case Mode::JoinOtherBlock:
        case Mode::DeleteBRElement:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other Run()");
          return EditActionResult(NS_ERROR_UNEXPECTED);
        case Mode::JoinBlocksInSameParent: {
          EditActionResult result =
              JoinBlockElementsInSameParent(aHTMLEditor, aDirectionAndAmount,
                                            aStripWrappers, aRangesToDelete);
          NS_WARNING_ASSERTION(result.Succeeded(),
                               "AutoBlockElementsJoiner::"
                               "JoinBlockElementsInSameParent() failed");
          return result;
        }
        case Mode::DeleteContentInRanges: {
          EditActionResult result =
              DeleteContentInRanges(aHTMLEditor, aDirectionAndAmount,
                                    aStripWrappers, aRangesToDelete);
          NS_WARNING_ASSERTION(
              result.Succeeded(),
              "AutoBlockElementsJoiner::DeleteContentInRanges() failed");
          return result;
        }
        case Mode::DeleteNonCollapsedRanges: {
          EditActionResult result = HandleDeleteNonCollapsedRanges(
              aHTMLEditor, aDirectionAndAmount, aStripWrappers, aRangesToDelete,
              aSelectionWasCollapsed);
          NS_WARNING_ASSERTION(result.Succeeded(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteNonCollapsedRange() failed");
          return result;
        }
        case Mode::NotInitialized:
          MOZ_ASSERT_UNREACHABLE(
              "Call Run() after calling a preparation method");
          return EditActionIgnored();
      }
      return EditActionResult(NS_ERROR_NOT_INITIALIZED);
    }

    nsresult ComputeRangesToDelete(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
      switch (mMode) {
        case Mode::JoinCurrentBlock:
        case Mode::JoinOtherBlock:
        case Mode::DeleteBRElement:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other "
              "ComputeRangesToDelete()");
          return NS_ERROR_UNEXPECTED;
        case Mode::JoinBlocksInSameParent: {
          nsresult rv = ComputeRangesToJoinBlockElementsInSameParent(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToJoinBlockElementsInSameParent() failed");
          return rv;
        }
        case Mode::DeleteContentInRanges: {
          nsresult rv = ComputeRangesToDeleteContentInRanges(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "AutoBlockElementsJoiner::"
                               "ComputeRangesToDeleteContentInRanges() failed");
          return rv;
        }
        case Mode::DeleteNonCollapsedRanges: {
          nsresult rv = ComputeRangesToDeleteNonCollapsedRanges(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete,
              aSelectionWasCollapsed);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteNonCollapsedRanges() failed");
          return rv;
        }
        case Mode::NotInitialized:
          MOZ_ASSERT_UNREACHABLE(
              "Call ComputeRangesToDelete() after calling a preparation "
              "method");
          return NS_ERROR_NOT_INITIALIZED;
      }
      return NS_ERROR_NOT_INITIALIZED;
    }

    nsIContent* GetLeafContentInOtherBlockElement() const {
      MOZ_ASSERT(mMode == Mode::JoinOtherBlock);
      return mLeafContentInOtherBlock;
    }

   private:
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    HandleDeleteAtCurrentBlockBoundary(HTMLEditor& aHTMLEditor,
                                       const EditorDOMPoint& aCaretPoint);
    nsresult ComputeRangesToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    HandleDeleteAtOtherBlockBoundary(HTMLEditor& aHTMLEditor,
                                     nsIEditor::EDirection aDirectionAndAmount,
                                     nsIEditor::EStripWrappers aStripWrappers,
                                     const EditorDOMPoint& aCaretPoint,
                                     AutoRangeArray& aRangesToDelete);
    // FYI: This method may modify selection, but it won't cause running
    //      script because of `AutoHideSelectionChanges` which blocks
    //      selection change listeners and the selection change event
    //      dispatcher.
    MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult
    ComputeRangesToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    JoinBlockElementsInSameParent(HTMLEditor& aHTMLEditor,
                                  nsIEditor::EDirection aDirectionAndAmount,
                                  nsIEditor::EStripWrappers aStripWrappers,
                                  AutoRangeArray& aRangesToDelete);
    nsresult ComputeRangesToJoinBlockElementsInSameParent(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult DeleteBRElement(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aCaretPoint);
    nsresult ComputeRangesToDeleteBRElement(
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult DeleteContentInRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete);
    nsresult ComputeRangesToDeleteContentInRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    HandleDeleteNonCollapsedRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed);
    nsresult ComputeRangesToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const;

    /**
     * JoinNodesDeepWithTransaction() joins aLeftNode and aRightNode "deeply".
     * First, they are joined simply, then, new right node is assumed as the
     * child at length of the left node before joined and new left node is
     * assumed as its previous sibling.  Then, they will be joined again.
     * And then, these steps are repeated.
     *
     * @param aLeftContent    The node which will be removed form the tree.
     * @param aRightContent   The node which will be inserted the contents of
     *                        aRightContent.
     * @return                The point of the first child of the last right
     * node. The result is always set if this succeeded.
     */
    MOZ_CAN_RUN_SCRIPT Result<EditorDOMPoint, nsresult>
    JoinNodesDeepWithTransaction(HTMLEditor& aHTMLEditor,
                                 nsIContent& aLeftContent,
                                 nsIContent& aRightContent);

    /**
     * DeleteNodesEntirelyInRangeButKeepTableStructure() removes nodes which are
     * entirely in aRange.  Howevers, if some nodes are part of a table,
     * removes all children of them instead.  I.e., this does not make damage to
     * table structure at the range, but may remove table entirely if it's
     * in the range.
     *
     * @return                  true if inclusive ancestor block elements at
     *                          start and end of the range should be joined.
     */
    MOZ_CAN_RUN_SCRIPT Result<bool, nsresult>
    DeleteNodesEntirelyInRangeButKeepTableStructure(
        HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed);
    bool NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor,
        const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const;
    Result<bool, nsresult>
    ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const;

    /**
     * DeleteContentButKeepTableStructure() removes aContent if it's an element
     * which is part of a table structure.  If it's a part of table structure,
     * removes its all children recursively.  I.e., this may delete all of a
     * table, but won't break table structure partially.
     *
     * @param aContent            The content which or whose all children should
     *                            be removed.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
    DeleteContentButKeepTableStructure(HTMLEditor& aHTMLEditor,
                                       nsIContent& aContent);

    /**
     * DeleteTextAtStartAndEndOfRange() removes text if start and/or end of
     * aRange is in a text node.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
    DeleteTextAtStartAndEndOfRange(HTMLEditor& aHTMLEditor, nsRange& aRange);

    class MOZ_STACK_CLASS AutoInclusiveAncestorBlockElementsJoiner final {
     public:
      AutoInclusiveAncestorBlockElementsJoiner() = delete;
      AutoInclusiveAncestorBlockElementsJoiner(
          nsIContent& aInclusiveDescendantOfLeftBlockElement,
          nsIContent& aInclusiveDescendantOfRightBlockElement)
          : mInclusiveDescendantOfLeftBlockElement(
                aInclusiveDescendantOfLeftBlockElement),
            mInclusiveDescendantOfRightBlockElement(
                aInclusiveDescendantOfRightBlockElement),
            mCanJoinBlocks(false),
            mFallbackToDeleteLeafContent(false) {}

      bool IsSet() const { return mLeftBlockElement && mRightBlockElement; }
      bool IsSameBlockElement() const {
        return mLeftBlockElement && mLeftBlockElement == mRightBlockElement;
      }

      /**
       * Prepare for joining inclusive ancestor block elements.  When this
       * returns false, the deletion should be canceled.
       */
      Result<bool, nsresult> Prepare(const HTMLEditor& aHTMLEditor);

      /**
       * When this returns true, this can join the blocks with `Run()`.
       */
      bool CanJoinBlocks() const { return mCanJoinBlocks; }

      /**
       * When this returns true, `Run()` must return "ignored" so that
       * caller can skip calling `Run()`.  This is available only when
       * `CanJoinBlocks()` returns `true`.
       * TODO: This should be merged into `CanJoinBlocks()` in the future.
       */
      bool ShouldDeleteLeafContentInstead() const {
        MOZ_ASSERT(CanJoinBlocks());
        return mFallbackToDeleteLeafContent;
      }

      /**
       * ComputeRangesToDelete() extends aRangesToDelete includes the element
       * boundaries between joining blocks.  If they won't be joined, this
       * collapses the range to aCaretPoint.
       */
      nsresult ComputeRangesToDelete(const HTMLEditor& aHTMLEditor,
                                     const EditorDOMPoint& aCaretPoint,
                                     AutoRangeArray& aRangesToDelete) const;

      /**
       * Join inclusive ancestor block elements which are found by preceding
       * Preare() call.
       * The right element is always joined to the left element.
       * If the elements are the same type and not nested within each other,
       * JoinEditableNodesWithTransaction() is called (example, joining two
       * list items together into one).
       * If the elements are not the same type, or one is a descendant of the
       * other, we instead destroy the right block placing its children into
       * left block.
       */
      [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
      Run(HTMLEditor& aHTMLEditor);

     private:
      /**
       * This method returns true when
       * `MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`,
       * `MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()` and
       * `MergeFirstLineOfRightBlockElementIntoLeftBlockElement()` handle it
       * with the `if` block of their main blocks.
       */
      bool CanMergeLeftAndRightBlockElements() const {
        if (!IsSet()) {
          return false;
        }
        // `MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`
        if (mPointContainingTheOtherBlockElement.GetContainer() ==
            mRightBlockElement) {
          return mNewListElementTagNameOfRightListElement.isSome();
        }
        // `MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()`
        if (mPointContainingTheOtherBlockElement.GetContainer() ==
            mLeftBlockElement) {
          return mNewListElementTagNameOfRightListElement.isSome() &&
                 !mRightBlockElement->GetChildCount();
        }
        MOZ_ASSERT(!mPointContainingTheOtherBlockElement.IsSet());
        // `MergeFirstLineOfRightBlockElementIntoLeftBlockElement()`
        return mNewListElementTagNameOfRightListElement.isSome() ||
               mLeftBlockElement->NodeInfo()->NameAtom() ==
                   mRightBlockElement->NodeInfo()->NameAtom();
      }

      OwningNonNull<nsIContent> mInclusiveDescendantOfLeftBlockElement;
      OwningNonNull<nsIContent> mInclusiveDescendantOfRightBlockElement;
      RefPtr<Element> mLeftBlockElement;
      RefPtr<Element> mRightBlockElement;
      Maybe<nsAtom*> mNewListElementTagNameOfRightListElement;
      EditorDOMPoint mPointContainingTheOtherBlockElement;
      RefPtr<dom::HTMLBRElement> mPrecedingInvisibleBRElement;
      bool mCanJoinBlocks;
      bool mFallbackToDeleteLeafContent;
    };  // HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
        // AutoInclusiveAncestorBlockElementsJoiner

    enum class Mode {
      NotInitialized,
      JoinCurrentBlock,
      JoinOtherBlock,
      JoinBlocksInSameParent,
      DeleteBRElement,
      DeleteContentInRanges,
      DeleteNonCollapsedRanges,
    };
    AutoDeleteRangesHandler* mDeleteRangesHandler;
    const AutoDeleteRangesHandler& mDeleteRangesHandlerConst;
    nsCOMPtr<nsIContent> mLeftContent;
    nsCOMPtr<nsIContent> mRightContent;
    nsCOMPtr<nsIContent> mLeafContentInOtherBlock;
    RefPtr<dom::HTMLBRElement> mBRElement;
    Mode mMode = Mode::NotInitialized;
  };  // HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner

  class MOZ_STACK_CLASS AutoEmptyBlockAncestorDeleter final {
   public:
    /**
     * ScanEmptyBlockInclusiveAncestor() scans an inclusive ancestor element
     * which is empty and a block element.  Then, stores the result and
     * returns the found empty block element.
     *
     * @param aHTMLEditor         The HTMLEditor.
     * @param aStartContent       Start content to look for empty ancestors.
     * @param aEditingHostElement Current editing host.
     */
    [[nodiscard]] Element* ScanEmptyBlockInclusiveAncestor(
        const HTMLEditor& aHTMLEditor, nsIContent& aStartContent,
        Element& aEditingHostElement);

    /**
     * ComputeTargetRanges() computes "target ranges" for deleting
     * `mEmptyInclusiveAncestorBlockElement`.
     */
    nsresult ComputeTargetRanges(const HTMLEditor& aHTMLEditor,
                                 nsIEditor::EDirection aDirectionAndAmount,
                                 const Element& aEditingHost,
                                 AutoRangeArray& aRangesToDelete) const;

    /**
     * Deletes found empty block element by `ScanEmptyBlockInclusiveAncestor()`.
     * If found one is a list item element, calls
     * `MaybeInsertBRElementBeforeEmptyListItemElement()` before deleting
     * the list item element.
     * If found empty ancestor is not a list item element,
     * `GetNewCaretPoisition()` will be called to determine new caret position.
     * Finally, removes the empty block ancestor.
     *
     * @param aHTMLEditor         The HTMLEditor.
     * @param aDirectionAndAmount If found empty ancestor block is a list item
     *                            element, this is ignored.  Otherwise:
     *                            - If eNext, eNextWord or eToEndOfLine,
     *                              collapse Selection to after found empty
     *                              ancestor.
     *                            - If ePrevious, ePreviousWord or
     *                              eToBeginningOfLine, collapse Selection to
     *                              end of previous editable node.
     *                            - Otherwise, eNone is allowed but does
     *                              nothing.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT EditActionResult
    Run(HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount);

   private:
    /**
     * MaybeInsertBRElementBeforeEmptyListItemElement() inserts a `<br>` element
     * if `mEmptyInclusiveAncestorBlockElement` is a list item element which
     * is first editable element in its parent, and its grand parent is not a
     * list element, inserts a `<br>` element before the empty list item.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<RefPtr<Element>, nsresult>
    MaybeInsertBRElementBeforeEmptyListItemElement(HTMLEditor& aHTMLEditor);

    /**
     * GetNewCaretPoisition() returns new caret position after deleting
     * `mEmptyInclusiveAncestorBlockElement`.
     */
    [[nodiscard]] Result<EditorDOMPoint, nsresult> GetNewCaretPoisition(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount) const;

    RefPtr<Element> mEmptyInclusiveAncestorBlockElement;
  };  // HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter

  const AutoDeleteRangesHandler* const mParent;
  nsIEditor::EDirection mOriginalDirectionAndAmount;
  nsIEditor::EStripWrappers mOriginalStripWrappers;
};  // HTMLEditor::AutoDeleteRangesHandler

nsresult HTMLEditor::ComputeTargetRanges(
    nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // TODO: Handle the case to delete in a table.

  AutoDeleteRangesHandler deleteHandler;
  nsresult rv = deleteHandler.ComputeRangesToDelete(*this, aDirectionAndAmount,
                                                    aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::ComputeRangesToDelete() failed");
  return rv;
}

EditActionResult HTMLEditor::HandleDeleteSelection(
    nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aStripWrappers == nsIEditor::eStrip ||
             aStripWrappers == nsIEditor::eNoStrip);

  // Remember that we did a selection deletion.  Used by
  // CreateStyleForInsertText()
  TopLevelEditSubActionDataRef().mDidDeleteSelection = true;

  // If there is only padding `<br>` element for empty editor, cancel the
  // operation.
  if (mPaddingBRElementForEmptyEditor) {
    return EditActionCanceled();
  }

  // First check for table selection mode.  If so, hand off to table editor.
  ErrorResult error;
  if (RefPtr<Element> cellElement = GetFirstSelectedTableCellElement(error)) {
    error.SuppressException();
    nsresult rv = DeleteTableCellContentsWithTransaction();
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTableCellContentsWithTransaction() failed");
    return EditActionHandled(rv);
  }

  // Only when there is no selection range, GetFirstSelectedTableCellElement()
  // returns error and in this case, anyway we cannot handle delete selection.
  // So, let's return error here even though we haven't handled this.
  if (error.Failed()) {
    NS_WARNING("HTMLEditor::GetFirstSelectedTableCellElement() failed");
    return EditActionResult(error.StealNSResult());
  }

  AutoRangeArray rangesToDelete(*SelectionRefPtr());
  AutoDeleteRangesHandler deleteHandler;
  EditActionResult result = deleteHandler.Run(*this, aDirectionAndAmount,
                                              aStripWrappers, rangesToDelete);
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "AutoDeleteRangesHandler::Run() failed");
    return result;
  }

  // XXX At here, selection may have no range because of mutation event
  //     listeners can do anything so that we should just return NS_OK instead
  //     of returning error.
  EditorDOMPoint atNewStartOfSelection(
      EditorBase::GetStartPoint(*SelectionRefPtr()));
  if (NS_WARN_IF(!atNewStartOfSelection.IsSet())) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  if (atNewStartOfSelection.GetContainerAsContent()) {
    nsresult rv = DeleteMostAncestorMailCiteElementIfEmpty(
        MOZ_KnownLive(*atNewStartOfSelection.GetContainerAsContent()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::DeleteMostAncestorMailCiteElementIfEmpty() failed");
      return EditActionHandled(rv);
    }
  }
  return EditActionHandled();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDelete(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  mOriginalDirectionAndAmount = aDirectionAndAmount;
  mOriginalStripWrappers = nsIEditor::eNoStrip;

  if (aHTMLEditor.mPaddingBRElementForEmptyEditor) {
    nsresult rv = aRangesToDelete.Collapse(
        EditorRawDOMPoint(aHTMLEditor.mPaddingBRElementForEmptyEditor));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  SelectionWasCollapsed selectionWasCollapsed = aRangesToDelete.IsCollapsed()
                                                    ? SelectionWasCollapsed::Yes
                                                    : SelectionWasCollapsed::No;
  if (selectionWasCollapsed == SelectionWasCollapsed::Yes) {
    EditorDOMPoint startPoint(aRangesToDelete.GetStartPointOfFirstRange());
    if (NS_WARN_IF(!startPoint.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    RefPtr<Element> editingHost = aHTMLEditor.GetActiveEditingHost();
    if (NS_WARN_IF(!editingHost)) {
      return NS_ERROR_FAILURE;
    }
    if (startPoint.GetContainerAsContent()) {
      AutoEmptyBlockAncestorDeleter deleter;
      if (deleter.ScanEmptyBlockInclusiveAncestor(
              aHTMLEditor, *startPoint.GetContainerAsContent(), *editingHost)) {
        nsresult rv = deleter.ComputeTargetRanges(
            aHTMLEditor, aDirectionAndAmount, *editingHost, aRangesToDelete);
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            "AutoEmptyBlockAncestorDeleter::ComputeTargetRanges() failed");
        return rv;
      }
    }

    // We shouldn't update caret bidi level right now, but we need to check
    // whether the deletion will be canceled or not.
    AutoCaretBidiLevelManager bidiLevelManager(aHTMLEditor, aDirectionAndAmount,
                                               startPoint);
    if (bidiLevelManager.Failed()) {
      NS_WARNING(
          "EditorBase::AutoCaretBidiLevelManager failed to initialize itself");
      return NS_ERROR_FAILURE;
    }
    if (bidiLevelManager.Canceled()) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }

    // AutoRangeArray::ExtendAnchorFocusRangeFor() will use `nsFrameSelection`
    // to extend the range for deletion.  But if focus event doesn't receive
    // yet, ancestor isn't set.  So we must set root element of editor to
    // ancestor temporarily.
    AutoSetTemporaryAncestorLimiter autoSetter(
        aHTMLEditor, *aHTMLEditor.SelectionRefPtr(), *startPoint.GetContainer(),
        &aRangesToDelete);

    Result<nsIEditor::EDirection, nsresult> extendResult =
        aRangesToDelete.ExtendAnchorFocusRangeFor(aHTMLEditor,
                                                  aDirectionAndAmount);
    if (extendResult.isErr()) {
      NS_WARNING("AutoRangeArray::ExtendAnchorFocusRangeFor() failed");
      return extendResult.unwrapErr();
    }

    // For compatibility with other browsers, we should set target ranges
    // to start from and/or end after an atomic content rather than start
    // from preceding text node end nor end at following text node start.
    Result<bool, nsresult> shrunkenResult =
        aRangesToDelete.ShrinkRangesIfStartFromOrEndAfterAtomicContent(
            aHTMLEditor, aDirectionAndAmount,
            AutoRangeArray::IfSelectingOnlyOneAtomicContent::Collapse,
            editingHost);
    if (shrunkenResult.isErr()) {
      NS_WARNING(
          "AutoRangeArray::ShrinkRangesIfStartFromOrEndAfterAtomicContent() "
          "failed");
      return shrunkenResult.unwrapErr();
    }

    if (!shrunkenResult.inspect() || !aRangesToDelete.IsCollapsed()) {
      aDirectionAndAmount = extendResult.unwrap();
    }

    if (aDirectionAndAmount == nsIEditor::eNone) {
      MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
      if (!CanFallbackToDeleteRangesWithTransaction(aRangesToDelete)) {
        // XXX In this case, do we need to modify the range again?
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      nsresult rv = FallbackToComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aRangesToDelete);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::"
          "FallbackToComputeRangesToDeleteRangesWithTransaction() failed");
      return rv;
    }

    if (aRangesToDelete.IsCollapsed()) {
      EditorDOMPoint caretPoint(aRangesToDelete.GetStartPointOfFirstRange());
      if (NS_WARN_IF(!caretPoint.IsInContentNode())) {
        return NS_ERROR_FAILURE;
      }
      if (!EditorUtils::IsEditableContent(*caretPoint.ContainerAsContent(),
                                          EditorType::HTML)) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      WSRunScanner wsRunScannerAtCaret(aHTMLEditor, caretPoint);
      WSScanResult scanFromCaretPointResult =
          aDirectionAndAmount == nsIEditor::eNext
              ? wsRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                    caretPoint)
              : wsRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                    caretPoint);
      if (!scanFromCaretPointResult.GetContent()) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }

      if (scanFromCaretPointResult.ReachedBRElement()) {
        if (scanFromCaretPointResult.BRElementPtr() ==
            wsRunScannerAtCaret.GetEditingHost()) {
          return NS_OK;
        }
        if (!EditorUtils::IsEditableContent(
                *scanFromCaretPointResult.BRElementPtr(), EditorType::HTML)) {
          return NS_SUCCESS_DOM_NO_OPERATION;
        }
        if (!aHTMLEditor.IsVisibleBRElement(
                scanFromCaretPointResult.BRElementPtr())) {
          EditorDOMPoint newCaretPosition =
              aDirectionAndAmount == nsIEditor::eNext
                  ? EditorDOMPoint::After(
                        *scanFromCaretPointResult.BRElementPtr())
                  : EditorDOMPoint(scanFromCaretPointResult.BRElementPtr());
          if (NS_WARN_IF(!newCaretPosition.IsSet())) {
            return NS_ERROR_FAILURE;
          }
          AutoHideSelectionChanges blockSelectionListeners(
              aHTMLEditor.SelectionRefPtr());
          nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPosition);
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::CollapseSelectionTo() failed");
            return NS_ERROR_FAILURE;
          }
          if (NS_WARN_IF(!aHTMLEditor.SelectionRefPtr()->RangeCount())) {
            return NS_ERROR_UNEXPECTED;
          }
          aRangesToDelete.Initialize(*aHTMLEditor.SelectionRefPtr());
          AutoDeleteRangesHandler anotherHandler(this);
          rv = anotherHandler.ComputeRangesToDelete(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "Recursive AutoDeleteRangesHandler::ComputeRangesToDelete() "
              "failed");

          DebugOnly<nsresult> rvIgnored =
              aHTMLEditor.CollapseSelectionTo(caretPoint);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                               "HTMLEditor::CollapseSelectionTo() failed to "
                               "restore original selection");

          MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
          // If the range is collapsed, there is no content which should
          // be removed together.  In this case, only the invisible `<br>`
          // element should be selected.
          if (aRangesToDelete.IsCollapsed()) {
            nsresult rv = aRangesToDelete.SelectNode(
                *scanFromCaretPointResult.BRElementPtr());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "AutoRangeArray::SelectNode() failed");
            return rv;
          }

          // Otherwise, extend the range to contain the invisible `<br>`
          // element.
          if (EditorRawDOMPoint(scanFromCaretPointResult.BRElementPtr())
                  .IsBefore(aRangesToDelete.GetStartPointOfFirstRange())) {
            nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
                EditorRawDOMPoint(scanFromCaretPointResult.BRElementPtr())
                    .ToRawRangeBoundary(),
                aRangesToDelete.FirstRangeRef()->EndRef());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "nsRange::SetStartAndEnd() failed");
            return rv;
          }
          if (aRangesToDelete.GetEndPointOfFirstRange().IsBefore(
                  EditorRawDOMPoint::After(
                      *scanFromCaretPointResult.BRElementPtr()))) {
            nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
                aRangesToDelete.FirstRangeRef()->StartRef(),
                EditorRawDOMPoint::After(
                    *scanFromCaretPointResult.BRElementPtr())
                    .ToRawRangeBoundary());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "nsRange::SetStartAndEnd() failed");
            return rv;
          }
          NS_WARNING("Was the invisible `<br>` element selected?");
          return NS_OK;
        }
      }

      nsresult rv = ComputeRangesToDeleteAroundCollapsedRanges(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete,
          wsRunScannerAtCaret, scanFromCaretPointResult);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::ComputeRangesToDeleteAroundCollapsedRanges("
          ") failed");
      return rv;
    }
  }

  nsresult rv = ComputeRangesToDeleteNonCollapsedRanges(
      aHTMLEditor, aDirectionAndAmount, aRangesToDelete, selectionWasCollapsed);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteNonCollapsedRanges() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::Run(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aStripWrappers == nsIEditor::eStrip ||
             aStripWrappers == nsIEditor::eNoStrip);
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  mOriginalDirectionAndAmount = aDirectionAndAmount;
  mOriginalStripWrappers = aStripWrappers;

  // If there is only padding `<br>` element for empty editor, cancel the
  // operation.
  if (aHTMLEditor.mPaddingBRElementForEmptyEditor) {
    return EditActionCanceled();
  }

  // selectionWasCollapsed is used later to determine whether we should join
  // blocks in HandleDeleteNonCollapsedRanges(). We don't really care about
  // collapsed because it will be modified by
  // AutoRangeArray::ExtendAnchorFocusRangeFor() later.
  // AutoBlockElementsJoiner::AutoInclusiveAncestorBlockElementsJoiner should
  // happen if the original selection is collapsed and the cursor is at the end
  // of a block element, in which case
  // AutoRangeArray::ExtendAnchorFocusRangeFor() would always make the selection
  // not collapsed.
  SelectionWasCollapsed selectionWasCollapsed = aRangesToDelete.IsCollapsed()
                                                    ? SelectionWasCollapsed::Yes
                                                    : SelectionWasCollapsed::No;

  if (selectionWasCollapsed == SelectionWasCollapsed::Yes) {
    EditorDOMPoint startPoint(aRangesToDelete.GetStartPointOfFirstRange());
    if (NS_WARN_IF(!startPoint.IsSet())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }

    // If we are inside an empty block, delete it.
    RefPtr<Element> editingHost = aHTMLEditor.GetActiveEditingHost();
    if (startPoint.GetContainerAsContent()) {
      if (NS_WARN_IF(!editingHost)) {
        return EditActionResult(NS_ERROR_FAILURE);
      }
#ifdef DEBUG
      nsMutationGuard debugMutation;
#endif  // #ifdef DEBUG
      AutoEmptyBlockAncestorDeleter deleter;
      if (deleter.ScanEmptyBlockInclusiveAncestor(
              aHTMLEditor, *startPoint.GetContainerAsContent(), *editingHost)) {
        EditActionResult result = deleter.Run(aHTMLEditor, aDirectionAndAmount);
        if (result.Failed() || result.Handled()) {
          NS_WARNING_ASSERTION(result.Succeeded(),
                               "AutoEmptyBlockAncestorDeleter::Run() failed");
          return result;
        }
      }
      MOZ_ASSERT(!debugMutation.Mutated(0),
                 "AutoEmptyBlockAncestorDeleter shouldn't modify the DOM tree "
                 "if it returns not handled nor error");
    }

    // Test for distance between caret and text that will be deleted.
    // Note that this call modifies `nsFrameSelection` without modifying
    // `Selection`.  However, it does not have problem for now because
    // it'll be referred by `AutoRangeArray::ExtendAnchorFocusRangeFor()`
    // before modifying `Selection`.
    // XXX This looks odd.  `ExtendAnchorFocusRangeFor()` will extend
    //     anchor-focus range, but here refers the first range.
    AutoCaretBidiLevelManager bidiLevelManager(aHTMLEditor, aDirectionAndAmount,
                                               startPoint);
    if (bidiLevelManager.Failed()) {
      NS_WARNING(
          "EditorBase::AutoCaretBidiLevelManager failed to initialize itself");
      return EditActionResult(NS_ERROR_FAILURE);
    }
    bidiLevelManager.MaybeUpdateCaretBidiLevel(aHTMLEditor);
    if (bidiLevelManager.Canceled()) {
      return EditActionCanceled();
    }

    // AutoRangeArray::ExtendAnchorFocusRangeFor() will use `nsFrameSelection`
    // to extend the range for deletion.  But if focus event doesn't receive
    // yet, ancestor isn't set.  So we must set root element of editor to
    // ancestor temporarily.
    AutoSetTemporaryAncestorLimiter autoSetter(
        aHTMLEditor, *aHTMLEditor.SelectionRefPtr(), *startPoint.GetContainer(),
        &aRangesToDelete);

    // Calling `ExtendAnchorFocusRangeFor()` and
    // `ShrinkRangesIfStartFromOrEndAfterAtomicContent()` may move caret to
    // the container of deleting atomic content.  However, it may be different
    // from the original caret's container.  The original caret container may
    // be important to put caret after deletion so that let's cache the
    // original position.
    Maybe<EditorDOMPoint> caretPoint;
    if (aRangesToDelete.IsCollapsed() && !aRangesToDelete.Ranges().IsEmpty()) {
      caretPoint = Some(aRangesToDelete.GetStartPointOfFirstRange());
      if (NS_WARN_IF(!caretPoint.ref().IsInContentNode())) {
        return EditActionResult(NS_ERROR_FAILURE);
      }
    }

    Result<nsIEditor::EDirection, nsresult> extendResult =
        aRangesToDelete.ExtendAnchorFocusRangeFor(aHTMLEditor,
                                                  aDirectionAndAmount);
    if (extendResult.isErr()) {
      NS_WARNING("AutoRangeArray::ExtendAnchorFocusRangeFor() failed");
      return EditActionResult(extendResult.unwrapErr());
    }
    if (caretPoint.isSome() && !caretPoint.ref().IsSetAndValid()) {
      NS_WARNING("The caret position became invalid");
      return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }

    // If there is only one range and it selects an atomic content, we should
    // delete it with collapsed range path for making consistent behavior
    // between both cases, the content is selected case and caret is at it or
    // after it case.
    Result<bool, nsresult> shrunkenResult =
        aRangesToDelete.ShrinkRangesIfStartFromOrEndAfterAtomicContent(
            aHTMLEditor, aDirectionAndAmount,
            AutoRangeArray::IfSelectingOnlyOneAtomicContent::Collapse,
            editingHost);
    if (shrunkenResult.isErr()) {
      NS_WARNING(
          "AutoRangeArray::ShrinkRangesIfStartFromOrEndAfterAtomicContent() "
          "failed");
      return EditActionResult(shrunkenResult.unwrapErr());
    }

    if (!shrunkenResult.inspect() || !aRangesToDelete.IsCollapsed()) {
      aDirectionAndAmount = extendResult.unwrap();
    }

    if (aDirectionAndAmount == nsIEditor::eNone) {
      MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
      if (!CanFallbackToDeleteRangesWithTransaction(aRangesToDelete)) {
        return EditActionIgnored();
      }
      EditActionResult result =
          FallbackToDeleteRangesWithTransaction(aHTMLEditor, aRangesToDelete);
      NS_WARNING_ASSERTION(result.Succeeded(),
                           "AutoDeleteRangesHandler::"
                           "FallbackToDeleteRangesWithTransaction() failed");
      return result;
    }

    if (aRangesToDelete.IsCollapsed()) {
      // Use the original caret position for handling the deletion around
      // collapsed range because the container may be different from the
      // new collapsed position's container.
      if (!EditorUtils::IsEditableContent(
              *caretPoint.ref().ContainerAsContent(), EditorType::HTML)) {
        return EditActionCanceled();
      }
      WSRunScanner wsRunScannerAtCaret(aHTMLEditor, caretPoint.ref());
      WSScanResult scanFromCaretPointResult =
          aDirectionAndAmount == nsIEditor::eNext
              ? wsRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                    caretPoint.ref())
              : wsRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                    caretPoint.ref());
      if (!scanFromCaretPointResult.GetContent()) {
        return EditActionCanceled();
      }
      // Short circuit for invisible breaks.  delete them and recurse.
      if (scanFromCaretPointResult.ReachedBRElement()) {
        if (scanFromCaretPointResult.BRElementPtr() ==
            wsRunScannerAtCaret.GetEditingHost()) {
          return EditActionHandled();
        }
        if (!EditorUtils::IsEditableContent(
                *scanFromCaretPointResult.BRElementPtr(), EditorType::HTML)) {
          return EditActionCanceled();
        }
        if (!aHTMLEditor.IsVisibleBRElement(
                scanFromCaretPointResult.BRElementPtr())) {
          // TODO: We should extend the range to delete again before/after
          //       the caret point and use `HandleDeleteNonCollapsedRanges()`
          //       instead after we would create delete range computation
          //       method at switching to the new white-space normalizer.
          nsresult rv = WhiteSpaceVisibilityKeeper::
              DeleteContentNodeAndJoinTextNodesAroundIt(
                  aHTMLEditor,
                  MOZ_KnownLive(*scanFromCaretPointResult.BRElementPtr()),
                  caretPoint.ref());
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "WhiteSpaceVisibilityKeeper::"
                "DeleteContentNodeAndJoinTextNodesAroundIt() failed");
            return EditActionHandled(rv);
          }
          if (aHTMLEditor.SelectionRefPtr()->RangeCount() != 1) {
            NS_WARNING(
                "Selection was unexpected after removing an invisible `<br>` "
                "element");
            return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
          }
          AutoRangeArray rangesToDelete(*aHTMLEditor.SelectionRefPtr());
          caretPoint = Some(aRangesToDelete.GetStartPointOfFirstRange());
          if (!caretPoint.ref().IsSet()) {
            NS_WARNING(
                "New selection after deleting invisible `<br>` element was "
                "invalid");
            return EditActionHandled(NS_ERROR_FAILURE);
          }
          if (aHTMLEditor.MaybeHasMutationEventListeners(
                  NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED |
                  NS_EVENT_BITS_MUTATION_NODEREMOVED |
                  NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT)) {
            // Let's check whether there is new invisible `<br>` element
            // for avoiding infinit recursive calls.
            WSRunScanner wsRunScannerAtCaret(aHTMLEditor, caretPoint.ref());
            WSScanResult scanFromCaretPointResult =
                aDirectionAndAmount == nsIEditor::eNext
                    ? wsRunScannerAtCaret
                          .ScanNextVisibleNodeOrBlockBoundaryFrom(
                              caretPoint.ref())
                    : wsRunScannerAtCaret
                          .ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                              caretPoint.ref());
            if (scanFromCaretPointResult.ReachedBRElement() &&
                !aHTMLEditor.IsVisibleBRElement(
                    scanFromCaretPointResult.BRElementPtr())) {
              return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
            }
          }
          AutoDeleteRangesHandler anotherHandler(this);
          EditActionResult result = anotherHandler.Run(
              aHTMLEditor, aDirectionAndAmount, aStripWrappers, rangesToDelete);
          NS_WARNING_ASSERTION(
              result.Succeeded(),
              "Recursive AutoDeleteRangesHandler::Run() failed");
          return result;
        }
      }

      EditActionResult result = HandleDeleteAroundCollapsedRanges(
          aHTMLEditor, aDirectionAndAmount, aStripWrappers, aRangesToDelete,
          wsRunScannerAtCaret, scanFromCaretPointResult);
      NS_WARNING_ASSERTION(result.Succeeded(),
                           "AutoDeleteRangesHandler::"
                           "HandleDeleteAroundCollapsedRanges() failed");
      return result;
    }
  }

  EditActionResult result = HandleDeleteNonCollapsedRanges(
      aHTMLEditor, aDirectionAndAmount, aStripWrappers, aRangesToDelete,
      selectionWasCollapsed);
  NS_WARNING_ASSERTION(
      result.Succeeded(),
      "AutoDeleteRangesHandler::HandleDeleteNonCollapsedRanges() failed");
  return result;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteAroundCollapsedRanges(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete, const WSRunScanner& aWSRunScannerAtCaret,
    const WSScanResult& aScanFromCaretPointResult) const {
  if (aScanFromCaretPointResult.InNormalWhiteSpaces() ||
      aScanFromCaretPointResult.InNormalText()) {
    nsresult rv = aRangesToDelete.Collapse(aScanFromCaretPointResult.Point());
    if (NS_FAILED(rv)) {
      NS_WARNING("AutoRangeArray::Collapse() failed");
      return NS_ERROR_FAILURE;
    }
    rv = ComputeRangesToDeleteTextAroundCollapsedRanges(
        aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::"
        "ComputeRangesToDeleteTextAroundCollapsedRanges() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedSpecialContent() ||
      aScanFromCaretPointResult.ReachedBRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return NS_OK;
    }
    nsresult rv = ComputeRangesToDeleteAtomicContent(
        aHTMLEditor, *aScanFromCaretPointResult.GetContent(), aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedHRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return NS_OK;
    }
    nsresult rv =
        ComputeRangesToDeleteHRElement(aHTMLEditor, aDirectionAndAmount,
                                       *aScanFromCaretPointResult.ElementPtr(),
                                       aWSRunScannerAtCaret.ScanStartRef(),
                                       aWSRunScannerAtCaret, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteHRElement() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedOtherBlockElement()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return NS_ERROR_FAILURE;
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtOtherBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret)) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }
    nsresult rv = joiner.ComputeRangesToDelete(
        aHTMLEditor, aDirectionAndAmount, aWSRunScannerAtCaret.ScanStartRef(),
        aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoBlockElementsJoiner::ComputeRangesToDelete() "
                         "failed (other block boundary)");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedCurrentBlockBoundary()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return NS_ERROR_FAILURE;
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtCurrentBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef())) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }
    nsresult rv = joiner.ComputeRangesToDelete(
        aHTMLEditor, aDirectionAndAmount, aWSRunScannerAtCaret.ScanStartRef(),
        aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoBlockElementsJoiner::ComputeRangesToDelete() "
                         "failed (current block boundary)");
    return rv;
  }

  return NS_OK;
}

EditActionResult
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteAroundCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
    const WSRunScanner& aWSRunScannerAtCaret,
    const WSScanResult& aScanFromCaretPointResult) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(aDirectionAndAmount != nsIEditor::eNone);
  MOZ_ASSERT(aWSRunScannerAtCaret.ScanStartRef().IsInContentNode());
  MOZ_ASSERT(EditorUtils::IsEditableContent(
      *aWSRunScannerAtCaret.ScanStartRef().ContainerAsContent(),
      EditorType::HTML));

  if (StaticPrefs::editor_white_space_normalization_blink_compatible()) {
    if (aScanFromCaretPointResult.InNormalWhiteSpaces() ||
        aScanFromCaretPointResult.InNormalText()) {
      nsresult rv = aRangesToDelete.Collapse(aScanFromCaretPointResult.Point());
      if (NS_FAILED(rv)) {
        NS_WARNING("AutoRangeArray::Collapse() failed");
        return EditActionResult(NS_ERROR_FAILURE);
      }
      EditActionResult result = HandleDeleteTextAroundCollapsedRanges(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
      NS_WARNING_ASSERTION(result.Succeeded(),
                           "AutoDeleteRangesHandler::"
                           "HandleDeleteTextAroundCollapsedRanges() failed");
      return result;
    }
  }

  if (aScanFromCaretPointResult.InNormalWhiteSpaces()) {
    EditActionResult result = HandleDeleteCollapsedSelectionAtWhiteSpaces(
        aHTMLEditor, aDirectionAndAmount, aWSRunScannerAtCaret.ScanStartRef());
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "AutoDeleteRangesHandler::"
                         "HandleDelectCollapsedSelectionAtWhiteSpaces() "
                         "failed");
    return result;
  }

  if (aScanFromCaretPointResult.InNormalText()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsText())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    EditActionResult result = HandleDeleteCollapsedSelectionAtVisibleChar(
        aHTMLEditor, aDirectionAndAmount, aScanFromCaretPointResult.Point());
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "AutoDeleteRangesHandler::"
                         "HandleDeleteCollapsedSelectionAtVisibleChar() "
                         "failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedSpecialContent() ||
      aScanFromCaretPointResult.ReachedBRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return EditActionHandled();
    }
    EditActionResult result = HandleDeleteAtomicContent(
        aHTMLEditor, MOZ_KnownLive(*aScanFromCaretPointResult.GetContent()),
        aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoDeleteRangesHandler::HandleDeleteAtomicContent() failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedHRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return EditActionHandled();
    }
    EditActionResult result = HandleDeleteHRElement(
        aHTMLEditor, aDirectionAndAmount,
        MOZ_KnownLive(*aScanFromCaretPointResult.ElementPtr()),
        aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoDeleteRangesHandler::HandleDeleteHRElement() failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedOtherBlockElement()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtOtherBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret)) {
      return EditActionCanceled();
    }
    EditActionResult result =
        joiner.Run(aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                   aWSRunScannerAtCaret.ScanStartRef(), aRangesToDelete);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoBlockElementsJoiner::Run() failed (other block boundary)");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedCurrentBlockBoundary()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtCurrentBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef())) {
      return EditActionCanceled();
    }
    EditActionResult result =
        joiner.Run(aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                   aWSRunScannerAtCaret.ScanStartRef(), aRangesToDelete);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoBlockElementsJoiner::Run() failed (current block boundary)");
    return result;
  }

  MOZ_ASSERT_UNREACHABLE("New type of reached content hasn't been handled yet");
  return EditActionIgnored();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::
    ComputeRangesToDeleteTextAroundCollapsedRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aDirectionAndAmount == nsIEditor::eNext ||
             aDirectionAndAmount == nsIEditor::ePrevious);

  EditorDOMPoint caretPosition(aRangesToDelete.GetStartPointOfFirstRange());
  MOZ_ASSERT(caretPosition.IsSetAndValid());
  if (NS_WARN_IF(!caretPosition.IsInContentNode())) {
    return NS_ERROR_FAILURE;
  }

  EditorDOMRangeInTexts rangeToDelete;
  if (aDirectionAndAmount == nsIEditor::eNext) {
    Result<EditorDOMRangeInTexts, nsresult> result =
        WSRunScanner::GetRangeInTextNodesToForwardDeleteFrom(aHTMLEditor,
                                                             caretPosition);
    if (result.isErr()) {
      NS_WARNING(
          "WSRunScanner::GetRangeInTextNodesToForwardDeleteFrom() failed");
      return result.unwrapErr();
    }
    rangeToDelete = result.unwrap();
    if (!rangeToDelete.IsPositioned()) {
      return NS_OK;  // no range to delete, but consume it.
    }
  } else {
    Result<EditorDOMRangeInTexts, nsresult> result =
        WSRunScanner::GetRangeInTextNodesToBackspaceFrom(aHTMLEditor,
                                                         caretPosition);
    if (result.isErr()) {
      NS_WARNING("WSRunScanner::GetRangeInTextNodesToBackspaceFrom() failed");
      return result.unwrapErr();
    }
    rangeToDelete = result.unwrap();
    if (!rangeToDelete.IsPositioned()) {
      return NS_OK;  // no range to delete, but consume it.
    }
  }

  nsresult rv = aRangesToDelete.SetStartAndEnd(rangeToDelete.StartRef(),
                                               rangeToDelete.EndRef());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoArrayRanges::SetStartAndEnd() failed");
  return rv;
}

EditActionResult
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteTextAroundCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aDirectionAndAmount == nsIEditor::eNext ||
             aDirectionAndAmount == nsIEditor::ePrevious);

  nsresult rv = ComputeRangesToDeleteTextAroundCollapsedRanges(
      aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
  if (NS_FAILED(rv)) {
    return EditActionResult(NS_ERROR_FAILURE);
  }
  if (aRangesToDelete.IsCollapsed()) {
    return EditActionHandled();  // no range to delete
  }

  // FYI: rangeToDelete does not contain newly empty inline ancestors which
  //      are removed by DeleteTextAndNormalizeSurroundingWhiteSpaces().
  //      So, if `getTargetRanges()` needs to include parent empty elements,
  //      we need to extend the range with
  //      HTMLEditUtils::GetMostDistantAnscestorEditableEmptyInlineElement().
  EditorRawDOMRange rangeToDelete(aRangesToDelete.FirstRangeRef());
  if (!rangeToDelete.IsInTextNodes()) {
    NS_WARNING("The extended range to delete character was not in text nodes");
    return EditActionResult(NS_ERROR_FAILURE);
  }

  AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
  Result<EditorDOMPoint, nsresult> result =
      aHTMLEditor.DeleteTextAndNormalizeSurroundingWhiteSpaces(
          rangeToDelete.StartRef().AsInText(),
          rangeToDelete.EndRef().AsInText(),
          TreatEmptyTextNodes::RemoveAllEmptyInlineAncestors,
          aDirectionAndAmount == nsIEditor::eNext ? DeleteDirection::Forward
                                                  : DeleteDirection::Backward);
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidNormalizeWhitespaces = true;
  if (result.isErr()) {
    NS_WARNING(
        "HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces() failed");
    return EditActionHandled(result.unwrapErr());
  }
  const EditorDOMPoint& newCaretPosition = result.inspect();
  MOZ_ASSERT(newCaretPosition.IsSetAndValid());

  DebugOnly<nsresult> rvIgnored =
      aHTMLEditor.SelectionRefPtr()->CollapseInLimiter(
          newCaretPosition.ToRawRangeBoundary());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "Selection::Collapse() failed, but ignored");
  return EditActionHandled();
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::
    HandleDeleteCollapsedSelectionAtWhiteSpaces(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aPointToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!StaticPrefs::editor_white_space_normalization_blink_compatible());

  if (aDirectionAndAmount == nsIEditor::eNext) {
    nsresult rv = WhiteSpaceVisibilityKeeper::DeleteInclusiveNextWhiteSpace(
        aHTMLEditor, aPointToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::DeleteInclusiveNextWhiteSpace() failed");
      return EditActionHandled(rv);
    }
  } else {
    nsresult rv = WhiteSpaceVisibilityKeeper::DeletePreviousWhiteSpace(
        aHTMLEditor, aPointToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::DeletePreviousWhiteSpace() failed");
      return EditActionHandled(rv);
    }
  }
  nsresult rv =
      aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
          EditorBase::GetStartPoint(*aHTMLEditor.SelectionRefPtr()));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() "
      "failed");
  return EditActionHandled(rv);
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::
    HandleDeleteCollapsedSelectionAtVisibleChar(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aPointToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!StaticPrefs::editor_white_space_normalization_blink_compatible());
  MOZ_ASSERT(aPointToDelete.IsSet());
  MOZ_ASSERT(aPointToDelete.IsInTextNode());

  OwningNonNull<Text> visibleTextNode = *aPointToDelete.GetContainerAsText();
  EditorDOMPoint startToDelete, endToDelete;
  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    if (aPointToDelete.IsStartOfContainer()) {
      return EditActionResult(NS_ERROR_UNEXPECTED);
    }
    startToDelete = aPointToDelete.PreviousPoint();
    endToDelete = aPointToDelete;
    // Bug 1068979: delete both codepoints if surrogate pair
    if (!startToDelete.IsStartOfContainer()) {
      const nsTextFragment* text = &visibleTextNode->TextFragment();
      if (text->IsLowSurrogateFollowingHighSurrogateAt(
              startToDelete.Offset())) {
        startToDelete.RewindOffset();
      }
    }
  } else {
    RefPtr<const nsRange> range = aHTMLEditor.SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!range) ||
        NS_WARN_IF(range->GetStartContainer() !=
                   aPointToDelete.GetContainer()) ||
        NS_WARN_IF(range->GetEndContainer() != aPointToDelete.GetContainer())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    startToDelete = range->StartRef();
    endToDelete = range->EndRef();
  }
  nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRangeAndTrackPoints(
      aHTMLEditor, &startToDelete, &endToDelete);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToDeleteRangeAndTrackPoints() "
        "failed");
    return EditActionResult(rv);
  }
  if (aHTMLEditor.MaybeHasMutationEventListeners(
          NS_EVENT_BITS_MUTATION_NODEREMOVED |
          NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
          NS_EVENT_BITS_MUTATION_ATTRMODIFIED |
          NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
      (NS_WARN_IF(!startToDelete.IsSetAndValid()) ||
       NS_WARN_IF(!startToDelete.IsInTextNode()) ||
       NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
       NS_WARN_IF(!endToDelete.IsInTextNode()) ||
       NS_WARN_IF(startToDelete.ContainerAsText() != visibleTextNode) ||
       NS_WARN_IF(endToDelete.ContainerAsText() != visibleTextNode) ||
       NS_WARN_IF(startToDelete.Offset() >= endToDelete.Offset()))) {
    NS_WARNING("Mutation event listener changed the DOM tree");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  rv = aHTMLEditor.DeleteTextWithTransaction(
      visibleTextNode, startToDelete.Offset(),
      endToDelete.Offset() - startToDelete.Offset());
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
    return EditActionHandled(rv);
  }

  // XXX When Backspace key is pressed, Chromium removes following empty
  //     text nodes when removing the last character of the non-empty text
  //     node.  However, Edge never removes empty text nodes even if
  //     selection is in the following empty text node(s).  For now, we
  //     should keep our traditional behavior same as Edge for backward
  //     compatibility.
  // XXX When Delete key is pressed, Edge removes all preceding empty
  //     text nodes when removing the first character of the non-empty
  //     text node.  Chromium removes only selected empty text node and
  //     following empty text nodes and the first character of the
  //     non-empty text node.  For now, we should keep our traditional
  //     behavior same as Chromium for backward compatibility.

  rv = DeleteNodeIfInvisibleAndEditableTextNode(aHTMLEditor, visibleTextNode);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
      "failed, but ignored");

  // XXX `Selection` may be modified by mutation event listeners so
  //     that we should use EditorDOMPoint::AtEndOf(visibleTextNode)
  //     instead.  (Perhaps, we don't and/or shouldn't need to do this
  //     if the text node is preformatted.)
  rv = aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
      EditorBase::GetStartPoint(*aHTMLEditor.SelectionRefPtr()));
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary()"
        " failed");
    return EditActionHandled(rv);
  }

  // Remember that we did a ranged delete for the benefit of
  // AfterEditInner().
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange = true;

  return EditActionHandled();
}

Result<bool, nsresult>
HTMLEditor::AutoDeleteRangesHandler::ShouldDeleteHRElement(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (StaticPrefs::editor_hr_element_allow_to_delete_from_following_line()) {
    return true;
  }

  if (aDirectionAndAmount != nsIEditor::ePrevious) {
    return true;
  }

  // Only if the caret is positioned at the end-of-hr-line position, we
  // want to delete the <hr>.
  //
  // In other words, we only want to delete, if our selection position
  // (indicated by aCaretPoint) is the position directly
  // after the <hr>, on the same line as the <hr>.
  //
  // To detect this case we check:
  // aCaretPoint's container == parent of `<hr>` element
  // and
  // aCaretPoint's offset -1 == `<hr>` element offset
  // and
  // interline position is false (left)
  //
  // In any other case we set the position to aCaretPoint's container -1
  // and interlineposition to false, only moving the caret to the
  // end-of-hr-line position.
  EditorRawDOMPoint atHRElement(&aHRElement);

  ErrorResult error;
  bool interLineIsRight =
      aHTMLEditor.SelectionRefPtr()->GetInterlinePosition(error);
  if (error.Failed()) {
    NS_WARNING("Selection::GetInterlinePosition() failed");
    nsresult rv = error.StealNSResult();
    return Err(rv);
  }

  return !interLineIsRight &&
         aCaretPoint.GetContainer() == atHRElement.GetContainer() &&
         aCaretPoint.Offset() - 1 == atHRElement.Offset();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteHRElement(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret,
    AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(&aHRElement != aWSRunScannerAtCaret.GetEditingHost());

  Result<bool, nsresult> canDeleteHRElement = ShouldDeleteHRElement(
      aHTMLEditor, aDirectionAndAmount, aHRElement, aCaretPoint);
  if (canDeleteHRElement.isErr()) {
    NS_WARNING("AutoDeleteRangesHandler::ShouldDeleteHRElement() failed");
    return canDeleteHRElement.unwrapErr();
  }
  if (canDeleteHRElement.inspect()) {
    nsresult rv = ComputeRangesToDeleteAtomicContent(aHTMLEditor, aHRElement,
                                                     aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
    return rv;
  }

  WSScanResult forwardScanFromCaretResult =
      aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(aCaretPoint);
  if (!forwardScanFromCaretResult.ReachedBRElement()) {
    // Restore original caret position if we won't delete anyting.
    nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  // If we'll just move caret position, but if it's followed by a `<br>`
  // element, we'll delete it.
  nsresult rv = ComputeRangesToDeleteAtomicContent(
      aHTMLEditor, *forwardScanFromCaretResult.ElementPtr(), aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::HandleDeleteHRElement(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(&aHRElement != aWSRunScannerAtCaret.GetEditingHost());

  Result<bool, nsresult> canDeleteHRElement = ShouldDeleteHRElement(
      aHTMLEditor, aDirectionAndAmount, aHRElement, aCaretPoint);
  if (canDeleteHRElement.isErr()) {
    NS_WARNING("AutoDeleteRangesHandler::ShouldDeleteHRElement() failed");
    return EditActionHandled(canDeleteHRElement.unwrapErr());
  }
  if (canDeleteHRElement.inspect()) {
    EditActionResult result = HandleDeleteAtomicContent(
        aHTMLEditor, aHRElement, aCaretPoint, aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoDeleteRangesHandler::HandleDeleteAtomicContent() failed");
    return result;
  }

  // Go to the position after the <hr>, but to the end of the <hr> line
  // by setting the interline position to left.
  EditorDOMPoint atNextOfHRElement(EditorDOMPoint::After(aHRElement));
  NS_WARNING_ASSERTION(atNextOfHRElement.IsSet(),
                       "Failed to set after <hr> element");

  {
    AutoEditorDOMPointChildInvalidator lockOffset(atNextOfHRElement);

    nsresult rv = aHTMLEditor.CollapseSelectionTo(atNextOfHRElement);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  }

  IgnoredErrorResult ignoredError;
  aHTMLEditor.SelectionRefPtr()->SetInterlinePosition(false, ignoredError);
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "Selection::SetInterlinePosition(false) failed, but ignored");
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidExplicitlySetInterLine = true;

  // There is one exception to the move only case.  If the <hr> is
  // followed by a <br> we want to delete the <br>.

  WSScanResult forwardScanFromCaretResult =
      aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(aCaretPoint);
  if (!forwardScanFromCaretResult.ReachedBRElement()) {
    return EditActionHandled();
  }

  // Delete the <br>
  nsresult rv =
      WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt(
          aHTMLEditor,
          MOZ_KnownLive(*forwardScanFromCaretResult.BRElementPtr()),
          aCaretPoint);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "WhiteSpaceVisibilityKeeper::"
                       "DeleteContentNodeAndJoinTextNodesAroundIt() failed");
  return EditActionHandled(rv);
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent(
    const HTMLEditor& aHTMLEditor, const nsIContent& aAtomicContent,
    AutoRangeArray& aRangesToDelete) const {
  EditorDOMRange rangeToDelete =
      WSRunScanner::GetRangesForDeletingAtomicContent(aHTMLEditor,
                                                      aAtomicContent);
  if (!rangeToDelete.IsPositioned()) {
    NS_WARNING("WSRunScanner::GetRangeForDeleteAContentNode() failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv = aRangesToDelete.SetStartAndEnd(rangeToDelete.StartRef(),
                                               rangeToDelete.EndRef());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoRangeArray::SetStartAndEnd() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::HandleDeleteAtomicContent(
    HTMLEditor& aHTMLEditor, nsIContent& aAtomicContent,
    const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT_IF(aAtomicContent.IsHTMLElement(nsGkAtoms::br),
                aHTMLEditor.IsVisibleBRElement(&aAtomicContent));
  MOZ_ASSERT(&aAtomicContent != aWSRunScannerAtCaret.GetEditingHost());

  nsresult rv =
      WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt(
          aHTMLEditor, aAtomicContent, aCaretPoint);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt("
        ") failed");
    return EditActionHandled(rv);
  }

  rv = aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
      EditorBase::GetStartPoint(*aHTMLEditor.SelectionRefPtr()));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() "
      "failed");
  return EditActionHandled(rv);
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount, Element& aOtherBlockElement,
        const EditorDOMPoint& aCaretPoint,
        const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());

  mMode = Mode::JoinOtherBlock;

  // Make sure it's not a table element.  If so, cancel the operation
  // (translation: users cannot backspace or delete across table cells)
  if (HTMLEditUtils::IsAnyTableElement(&aOtherBlockElement)) {
    return false;
  }

  // First find the adjacent node in the block
  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    mLeafContentInOtherBlock =
        aHTMLEditor.GetLastEditableLeaf(aOtherBlockElement);
    mLeftContent = mLeafContentInOtherBlock;
    mRightContent = aCaretPoint.GetContainerAsContent();
  } else {
    mLeafContentInOtherBlock =
        aHTMLEditor.GetFirstEditableLeaf(aOtherBlockElement);
    mLeftContent = aCaretPoint.GetContainerAsContent();
    mRightContent = mLeafContentInOtherBlock;
  }

  // Next to a block.  See if we are between the block and a `<br>`.
  // If so, we really want to delete the `<br>`.  Else join content at
  // selection to the block.
  WSScanResult scanFromCaretResult =
      aDirectionAndAmount == nsIEditor::eNext
          ? aWSRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                aCaretPoint)
          : aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                aCaretPoint);
  // If we found a `<br>` element, we need to delete it instead of joining the
  // contents.
  if (scanFromCaretResult.ReachedBRElement()) {
    mBRElement = scanFromCaretResult.BRElementPtr();
    mMode = Mode::DeleteBRElement;
    return true;
  }

  return mLeftContent && mRightContent;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteBRElement(AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(mBRElement);
  // XXX Why don't we scan invisible leading white-spaces which follows the
  //     `<br>` element?
  nsresult rv = aRangesToDelete.SelectNode(*mBRElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::SelectNode() failed");
  return rv;
}

EditActionResult
HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::DeleteBRElement(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    const EditorDOMPoint& aCaretPoint) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mBRElement);

  // If we found a `<br>` element, we should delete it instead of joining the
  // contents.
  nsresult rv =
      aHTMLEditor.DeleteNodeWithTransaction(MOZ_KnownLive(*mBRElement));
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
    return EditActionResult(rv);
  }

  if (mLeftContent && mRightContent &&
      HTMLEditor::NodesInDifferentTableElements(*mLeftContent,
                                                *mRightContent)) {
    return EditActionHandled();
  }

  // Put selection at edge of block and we are done.
  if (NS_WARN_IF(!mLeafContentInOtherBlock)) {
    // XXX This must be odd case.  The other block can be empty.
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  EditorDOMPoint newCaretPosition = aHTMLEditor.GetGoodCaretPointFor(
      *mLeafContentInOtherBlock, aDirectionAndAmount);
  if (!newCaretPosition.IsSet()) {
    NS_WARNING("HTMLEditor::GetGoodCaretPointFor() failed");
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  rv = aHTMLEditor.CollapseSelectionTo(newCaretPosition);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  return EditActionHandled();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  if (HTMLEditor::NodesInDifferentTableElements(*mLeftContent,
                                                *mRightContent)) {
    if (!mDeleteRangesHandlerConst.CanFallbackToDeleteRangesWithTransaction(
            aRangesToDelete)) {
      nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::Collapse() failed");
      return rv;
    }
    nsresult rv = mDeleteRangesHandlerConst
                      .FallbackToComputeRangesToDeleteRangesWithTransaction(
                          aHTMLEditor, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::"
        "FallbackToComputeRangesToDeleteRangesWithTransaction() failed");
    return rv;
  }

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }
  if (canJoinThem.inspect() && joiner.CanJoinBlocks() &&
      !joiner.ShouldDeleteLeafContentInstead()) {
    nsresult rv =
        joiner.ComputeRangesToDelete(aHTMLEditor, aCaretPoint, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete() "
        "failed");
    return rv;
  }

  // If AutoInclusiveAncestorBlockElementsJoiner didn't handle it and it's not
  // canceled, user may want to modify the start leaf node or the last leaf
  // node of the block.
  if (mLeafContentInOtherBlock == aCaretPoint.GetContainer()) {
    return NS_OK;
  }

  AutoHideSelectionChanges hideSelectionChanges(aHTMLEditor.SelectionRefPtr());

  // If it's ignored, it didn't modify the DOM tree.  In this case, user must
  // want to delete nearest leaf node in the other block element.
  // TODO: We need to consider this before calling ComputeRangesToDelete() for
  //       computing the deleting range.
  EditorRawDOMPoint newCaretPoint =
      aDirectionAndAmount == nsIEditor::ePrevious
          ? EditorRawDOMPoint::AtEndOf(*mLeafContentInOtherBlock)
          : EditorRawDOMPoint(mLeafContentInOtherBlock, 0);
  // TODO: Stop modifying the `Selection` for computing the targer ranges.
  nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPoint);
  if (!aHTMLEditor.SelectionRefPtr()->RangeCount()) {
    NS_WARNING("Failed to put caret to new position");
    return NS_ERROR_FAILURE;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  aRangesToDelete.Initialize(*aHTMLEditor.SelectionRefPtr());
  AutoDeleteRangesHandler anotherHandler(mDeleteRangesHandlerConst);
  rv = anotherHandler.ComputeRangesToDelete(aHTMLEditor, aDirectionAndAmount,
                                            aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "Recursive AutoDeleteRangesHandler::ComputeRangesToDelete() failed");
  // Restore selection.
  nsresult rvCollapsingSelectionTo =
      aHTMLEditor.CollapseSelectionTo(aCaretPoint);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rvCollapsingSelectionTo),
      "HTMLEditor::CollapseSelectionTo() failed to restore caret position");
  return NS_SUCCEEDED(rv) && NS_SUCCEEDED(rvCollapsingSelectionTo)
             ? NS_OK
             : NS_ERROR_FAILURE;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    HandleDeleteAtOtherBlockBoundary(HTMLEditor& aHTMLEditor,
                                     nsIEditor::EDirection aDirectionAndAmount,
                                     nsIEditor::EStripWrappers aStripWrappers,
                                     const EditorDOMPoint& aCaretPoint,
                                     AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  if (HTMLEditor::NodesInDifferentTableElements(*mLeftContent,
                                                *mRightContent)) {
    // If we have not deleted `<br>` element and are not called recursively,
    // we should call `DeleteRangesWithTransaction()` here.
    if (!mDeleteRangesHandler->CanFallbackToDeleteRangesWithTransaction(
            aRangesToDelete)) {
      return EditActionIgnored();
    }
    EditActionResult result =
        mDeleteRangesHandler->FallbackToDeleteRangesWithTransaction(
            aHTMLEditor, aRangesToDelete);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "AutoDeleteRangesHandler::FallbackToDeleteRangesWithTransaction() "
        "failed to delete leaf content in the block");
    return result;
  }

  // Else we are joining content to block
  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return EditActionResult(canJoinThem.unwrapErr());
  }

  if (!canJoinThem.inspect()) {
    nsresult rv = aHTMLEditor.CollapseSelectionTo(aCaretPoint);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionTo() failed, but ignored");
    return EditActionCanceled();
  }

  EditActionResult result(NS_OK);
  EditorDOMPoint pointToPutCaret(aCaretPoint);
  if (joiner.CanJoinBlocks()) {
    {
      AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(),
                                &pointToPutCaret);
      result |= joiner.Run(aHTMLEditor);
      if (result.Failed()) {
        NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
        return result;
      }
#ifdef DEBUG
      if (joiner.ShouldDeleteLeafContentInstead()) {
        NS_ASSERTION(
            result.Ignored(),
            "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
            "retruning ignored, but returned not ignored");
      } else {
        NS_ASSERTION(
            !result.Ignored(),
            "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
            "retruning handled, but returned ignored");
      }
#endif  // #ifdef DEBUG
    }

    // If AutoInclusiveAncestorBlockElementsJoiner didn't handle it and it's not
    // canceled, user may want to modify the start leaf node or the last leaf
    // node of the block.
    if (result.Ignored() &&
        mLeafContentInOtherBlock != aCaretPoint.GetContainer()) {
      // If it's ignored, it didn't modify the DOM tree.  In this case, user
      // must want to delete nearest leaf node in the other block element.
      // TODO: We need to consider this before calling Run() for computing the
      //       deleting range.
      EditorRawDOMPoint newCaretPoint =
          aDirectionAndAmount == nsIEditor::ePrevious
              ? EditorRawDOMPoint::AtEndOf(*mLeafContentInOtherBlock)
              : EditorRawDOMPoint(mLeafContentInOtherBlock, 0);
      nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPoint);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return result.SetResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (!aHTMLEditor.SelectionRefPtr()->RangeCount()) {
        NS_WARNING("Failed to put caret to new position");
        return EditActionHandled(rv);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::CollapseSelectionTo() failed, but ignored");
      AutoRangeArray rangesToDelete(*aHTMLEditor.SelectionRefPtr());
      AutoDeleteRangesHandler anotherHandler(mDeleteRangesHandler);
      result = anotherHandler.Run(aHTMLEditor, aDirectionAndAmount,
                                  aStripWrappers, rangesToDelete);
      NS_WARNING_ASSERTION(result.Succeeded(),
                           "Recursive AutoDeleteRangesHandler::Run() failed");
      return result;
    }
  } else {
    result.MarkAsHandled();
  }

  // Otherwise, we must have deleted the selection as user expected.
  nsresult rv = aHTMLEditor.CollapseSelectionTo(pointToPutCaret);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return result.SetResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  return result;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        Element& aCurrentBlockElement, const EditorDOMPoint& aCaretPoint) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  // At edge of our block.  Look beside it and see if we can join to an
  // adjacent block
  mMode = Mode::JoinCurrentBlock;

  // Don't break the basic structure of the HTML document.
  if (aCurrentBlockElement.IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                               nsGkAtoms::body)) {
    return false;
  }

  // Make sure it's not a table element.  If so, cancel the operation
  // (translation: users cannot backspace or delete across table cells)
  if (HTMLEditUtils::IsAnyTableElement(&aCurrentBlockElement)) {
    return false;
  }

  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    mLeftContent =
        aHTMLEditor.GetPreviousEditableHTMLNode(aCurrentBlockElement);
    mRightContent = aCaretPoint.GetContainerAsContent();
  } else {
    mRightContent = aHTMLEditor.GetNextEditableHTMLNode(aCurrentBlockElement);
    mLeftContent = aCaretPoint.GetContainerAsContent();
  }

  // Nothing to join
  if (!mLeftContent || !mRightContent) {
    return false;
  }

  // Don't cross table boundaries.
  return !HTMLEditor::NodesInDifferentTableElements(*mLeftContent,
                                                    *mRightContent);
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }
  if (canJoinThem.inspect()) {
    nsresult rv =
        joiner.ComputeRangesToDelete(aHTMLEditor, aCaretPoint, aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoInclusiveAncestorBlockElementsJoiner::"
                         "ComputeRangesToDelete() failed");
    return rv;
  }

  // In this case, nothing will be deleted so that the affected range should
  // be collapsed.
  nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    HandleDeleteAtCurrentBlockBoundary(HTMLEditor& aHTMLEditor,
                                       const EditorDOMPoint& aCaretPoint) {
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return EditActionResult(canJoinThem.unwrapErr());
  }

  if (!canJoinThem.inspect()) {
    nsresult rv = aHTMLEditor.CollapseSelectionTo(aCaretPoint);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionTo() failed, but ignored");
    return EditActionCanceled();
  }

  EditActionResult result(NS_OK);
  EditorDOMPoint pointToPutCaret(aCaretPoint);
  if (joiner.CanJoinBlocks()) {
    AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(), &pointToPutCaret);
    result |= joiner.Run(aHTMLEditor);
    if (result.Failed()) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
      return result;
    }
#ifdef DEBUG
    if (joiner.ShouldDeleteLeafContentInstead()) {
      NS_ASSERTION(result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning ignored, but returned not ignored");
    } else {
      NS_ASSERTION(!result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning handled, but returned ignored");
    }
#endif  // #ifdef DEBUG
  }
  // This should claim that trying to join the block means that
  // this handles the action because the caller shouldn't do anything
  // anymore in this case.
  result.MarkAsHandled();

  nsresult rv = aHTMLEditor.CollapseSelectionTo(pointToPutCaret);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return result.SetResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  return result;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteNonCollapsedRanges(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete,
    AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
    const {
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->StartRef().IsSet()) ||
      NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->EndRef().IsSet())) {
    return NS_ERROR_FAILURE;
  }

  if (aRangesToDelete.Ranges().Length() == 1) {
    nsFrameSelection* frameSelection =
        aHTMLEditor.SelectionRefPtr()->GetFrameSelection();
    if (NS_WARN_IF(!frameSelection)) {
      return NS_ERROR_FAILURE;
    }
    if (!ExtendRangeToIncludeInvisibleNodes(aHTMLEditor, frameSelection,
                                            aRangesToDelete.FirstRangeRef())) {
      NS_WARNING(
          "AutoDeleteRangesHandler::ExtendRangeToIncludeInvisibleNodes() "
          "failed");
      return NS_ERROR_FAILURE;
    }
  }

  if (!aHTMLEditor.IsPlaintextEditor()) {
    EditorDOMRange firstRange(aRangesToDelete.FirstRangeRef());
    EditorDOMRange extendedRange =
        WSRunScanner::GetRangeContainingInvisibleWhiteSpacesAtRangeBoundaries(
            aHTMLEditor, EditorDOMRange(aRangesToDelete.FirstRangeRef()));
    if (firstRange != extendedRange) {
      nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
          extendedRange.StartRef().ToRawRangeBoundary(),
          extendedRange.EndRef().ToRawRangeBoundary());
      if (NS_FAILED(rv)) {
        NS_WARNING("nsRange::SetStartAndEnd() failed");
        return NS_ERROR_FAILURE;
      }
    }
  }

  if (aRangesToDelete.FirstRangeRef()->GetStartContainer() ==
      aRangesToDelete.FirstRangeRef()->GetEndContainer()) {
    if (!aRangesToDelete.FirstRangeRef()->Collapsed()) {
      nsresult rv = ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::ComputeRangesToDeleteRangesWithTransaction("
          ") failed");
      return rv;
    }
    // `DeleteUnnecessaryNodesAndCollapseSelection()` may delete parent
    // elements, but it does not affect computing target ranges.  Therefore,
    // we don't need to touch aRangesToDelete in this case.
    return NS_OK;
  }

  Element* startCiteNode = aHTMLEditor.GetMostAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetStartContainer());
  Element* endCiteNode = aHTMLEditor.GetMostAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetEndContainer());

  if (startCiteNode && !endCiteNode) {
    aDirectionAndAmount = nsIEditor::eNext;
  } else if (!startCiteNode && endCiteNode) {
    aDirectionAndAmount = nsIEditor::ePrevious;
  }

  AutoBlockElementsJoiner joiner(*this);
  if (!joiner.PrepareToDeleteNonCollapsedRanges(aHTMLEditor, aRangesToDelete)) {
    return NS_ERROR_FAILURE;
  }
  nsresult rv =
      joiner.ComputeRangesToDelete(aHTMLEditor, aDirectionAndAmount,
                                   aRangesToDelete, aSelectionWasCollapsed);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoBlockElementsJoiner::ComputeRangesToDelete() failed");
  return rv;
}

EditActionResult
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteNonCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
    SelectionWasCollapsed aSelectionWasCollapsed) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->StartRef().IsSet()) ||
      NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->EndRef().IsSet())) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  // Else we have a non-collapsed selection.  First adjust the selection.
  // XXX Why do we extend selection only when there is only one range?
  if (aRangesToDelete.Ranges().Length() == 1) {
    nsFrameSelection* frameSelection =
        aHTMLEditor.SelectionRefPtr()->GetFrameSelection();
    if (NS_WARN_IF(!frameSelection)) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    if (!ExtendRangeToIncludeInvisibleNodes(aHTMLEditor, frameSelection,
                                            aRangesToDelete.FirstRangeRef())) {
      NS_WARNING(
          "AutoDeleteRangesHandler::ExtendRangeToIncludeInvisibleNodes() "
          "failed");
      return EditActionResult(NS_ERROR_FAILURE);
    }
  }

  // Remember that we did a ranged delete for the benefit of AfterEditInner().
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange = true;

  // Figure out if the endpoints are in nodes that can be merged.  Adjust
  // surrounding white-space in preparation to delete selection.
  if (!aHTMLEditor.IsPlaintextEditor()) {
    AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
    AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                        &aRangesToDelete.FirstRangeRef());
    nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
        aHTMLEditor, EditorDOMRange(aRangesToDelete.FirstRangeRef()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() "
          "failed");
      return EditActionResult(rv);
    }
    if (NS_WARN_IF(
            !aRangesToDelete.FirstRangeRef()->StartRef().IsSetAndValid()) ||
        NS_WARN_IF(
            !aRangesToDelete.FirstRangeRef()->EndRef().IsSetAndValid())) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() made the firstr "
          "range invalid");
      return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }
  }

  // XXX This is odd.  We do we simply use `DeleteRangesWithTransaction()`
  //     only when **first** range is in same container?
  if (aRangesToDelete.FirstRangeRef()->GetStartContainer() ==
      aRangesToDelete.FirstRangeRef()->GetEndContainer()) {
    // Because of previous DOM tree changes, the range may be collapsed.
    // If we've already removed all contents in the range, we shouldn't
    // delete anything around the caret.
    if (!aRangesToDelete.FirstRangeRef()->Collapsed()) {
      AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                          &aRangesToDelete.FirstRangeRef());
      nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
          aDirectionAndAmount, aStripWrappers, aRangesToDelete);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteRangesWithTransaction() failed");
        return EditActionHandled(rv);
      }
    }
    // However, even if the range is removed, we may need to clean up the
    // containers which become empty.
    nsresult rv = DeleteUnnecessaryNodesAndCollapseSelection(
        aHTMLEditor, aDirectionAndAmount,
        EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
        EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoDeleteRangesHandler::"
                         "DeleteUnnecessaryNodesAndCollapseSelection() failed");
    return EditActionHandled(rv);
  }

  if (NS_WARN_IF(
          !aRangesToDelete.FirstRangeRef()->GetStartContainer()->IsContent()) ||
      NS_WARN_IF(
          !aRangesToDelete.FirstRangeRef()->GetEndContainer()->IsContent())) {
    return EditActionHandled(NS_ERROR_FAILURE);  // XXX "handled"?
  }

  // Figure out mailcite ancestors
  RefPtr<Element> startCiteNode = aHTMLEditor.GetMostAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetStartContainer());
  RefPtr<Element> endCiteNode = aHTMLEditor.GetMostAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetEndContainer());

  // If we only have a mailcite at one of the two endpoints, set the
  // directionality of the deletion so that the selection will end up
  // outside the mailcite.
  if (startCiteNode && !endCiteNode) {
    aDirectionAndAmount = nsIEditor::eNext;
  } else if (!startCiteNode && endCiteNode) {
    aDirectionAndAmount = nsIEditor::ePrevious;
  }

  AutoBlockElementsJoiner joiner(*this);
  if (!joiner.PrepareToDeleteNonCollapsedRanges(aHTMLEditor, aRangesToDelete)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  EditActionResult result =
      joiner.Run(aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                 aRangesToDelete, aSelectionWasCollapsed);
  NS_WARNING_ASSERTION(result.Succeeded(),
                       "AutoBlockElementsJoiner::Run() failed");
  return result;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteNonCollapsedRanges(const HTMLEditor& aHTMLEditor,
                                      const AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  mLeftContent = HTMLEditUtils::GetInclusiveAncestorBlockElement(
      *aRangesToDelete.FirstRangeRef()->GetStartContainer()->AsContent());
  mRightContent = HTMLEditUtils::GetInclusiveAncestorBlockElement(
      *aRangesToDelete.FirstRangeRef()->GetEndContainer()->AsContent());
  if (NS_WARN_IF(!mLeftContent) || NS_WARN_IF(!mRightContent)) {
    return false;
  }
  if (mLeftContent == mRightContent) {
    mMode = Mode::DeleteContentInRanges;
    return true;
  }

  // If left block and right block are adjuscent siblings and they are same
  // type of elements, we can merge them after deleting the selected contents.
  // MOOSE: this could conceivably screw up a table.. fix me.
  if (mLeftContent->GetParentNode() == mRightContent->GetParentNode() &&
      HTMLEditUtils::CanContentsBeJoined(
          *mLeftContent, *mRightContent,
          aHTMLEditor.IsCSSEnabled() ? StyleDifference::CompareIfSpanElements
                                     : StyleDifference::Ignore) &&
      // XXX What's special about these three types of block?
      (mLeftContent->IsHTMLElement(nsGkAtoms::p) ||
       HTMLEditUtils::IsListItem(mLeftContent) ||
       HTMLEditUtils::IsHeader(*mLeftContent))) {
    mMode = Mode::JoinBlocksInSameParent;
    return true;
  }

  mMode = Mode::DeleteNonCollapsedRanges;
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteContentInRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::DeleteContentInRanges);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  nsresult rv =
      mDeleteRangesHandlerConst.ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteRangesWithTransaction() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    DeleteContentInRanges(HTMLEditor& aHTMLEditor,
                          nsIEditor::EDirection aDirectionAndAmount,
                          nsIEditor::EStripWrappers aStripWrappers,
                          AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::DeleteContentInRanges);
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  // XXX This is also odd.  We do we simply use
  //     `DeleteRangesWithTransaction()` only when **first** range is in
  //     same block?
  MOZ_ASSERT(mLeftContent == mRightContent);
  {
    AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                        &aRangesToDelete.FirstRangeRef());
    nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
        aDirectionAndAmount, aStripWrappers, aRangesToDelete);
    if (rv == NS_ERROR_EDITOR_DESTROYED) {
      NS_WARNING(
          "EditorBase::DeleteRangesWithTransaction() caused destroying the "
          "editor");
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::DeleteRangesWithTransaction() failed, but ignored");
  }
  nsresult rv =
      mDeleteRangesHandler->DeleteUnnecessaryNodesAndCollapseSelection(
          aHTMLEditor, aDirectionAndAmount,
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "DeleteUnnecessaryNodesAndCollapseSelection() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToJoinBlockElementsInSameParent(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::JoinBlocksInSameParent);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT(mLeftContent->GetParentNode() == mRightContent->GetParentNode());

  nsresult rv =
      mDeleteRangesHandlerConst.ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteRangesWithTransaction() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    JoinBlockElementsInSameParent(HTMLEditor& aHTMLEditor,
                                  nsIEditor::EDirection aDirectionAndAmount,
                                  nsIEditor::EStripWrappers aStripWrappers,
                                  AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::JoinBlocksInSameParent);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT(mLeftContent->GetParentNode() == mRightContent->GetParentNode());

  nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
      aDirectionAndAmount, aStripWrappers, aRangesToDelete);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteRangesWithTransaction() failed");
    return EditActionHandled(rv);
  }

  Result<EditorDOMPoint, nsresult> atFirstChildOfTheLastRightNodeOrError =
      JoinNodesDeepWithTransaction(aHTMLEditor, MOZ_KnownLive(*mLeftContent),
                                   MOZ_KnownLive(*mRightContent));
  if (atFirstChildOfTheLastRightNodeOrError.isErr()) {
    NS_WARNING("HTMLEditor::JoinNodesDeepWithTransaction() failed");
    return EditActionHandled(atFirstChildOfTheLastRightNodeOrError.unwrapErr());
  }
  MOZ_ASSERT(atFirstChildOfTheLastRightNodeOrError.inspect().IsSet());

  rv = aHTMLEditor.CollapseSelectionTo(
      atFirstChildOfTheLastRightNodeOrError.inspect());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return EditActionHandled(rv);
}

Result<bool, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  AutoTArray<OwningNonNull<nsIContent>, 10> arrayOfTopChildren;
  DOMSubtreeIterator iter;
  nsresult rv = iter.Init(aRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("DOMSubtreeIterator::Init() failed");
    return Err(rv);
  }
  iter.AppendAllNodesToArray(arrayOfTopChildren);
  return NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
      aHTMLEditor, arrayOfTopChildren, aSelectionWasCollapsed);
}

Result<bool, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::DeleteNodesEntirelyInRangeButKeepTableStructure(
        HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  // Build a list of direct child nodes in the range
  AutoTArray<OwningNonNull<nsIContent>, 10> arrayOfTopChildren;
  DOMSubtreeIterator iter;
  nsresult rv = iter.Init(aRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("DOMSubtreeIterator::Init() failed");
    return Err(rv);
  }
  iter.AppendAllNodesToArray(arrayOfTopChildren);

  // Now that we have the list, delete non-table elements
  bool needsToJoinLater =
      NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
          aHTMLEditor, arrayOfTopChildren, aSelectionWasCollapsed);
  for (auto& content : arrayOfTopChildren) {
    // XXX After here, the child contents in the array may have been moved
    //     to somewhere or removed.  We should handle it.
    //
    // MOZ_KnownLive because 'arrayOfTopChildren' is guaranteed to
    // keep it alive.
    //
    // Even with https://bugzilla.mozilla.org/show_bug.cgi?id=1620312 fixed
    // this might need to stay, because 'arrayOfTopChildren' is not const,
    // so it's not obvious how to prove via static analysis that it won't
    // change and release us.
    nsresult rv =
        DeleteContentButKeepTableStructure(aHTMLEditor, MOZ_KnownLive(content));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoBlockElementsJoiner::DeleteContentButKeepTableStructure() failed, "
        "but ignored");
  }
  return needsToJoinLater;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor,
        const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
  // If original selection was collapsed, we need always to join the nodes.
  // XXX Why?
  if (aSelectionWasCollapsed ==
      AutoDeleteRangesHandler::SelectionWasCollapsed::No) {
    return true;
  }
  // If something visible is deleted, no need to join.  Visible means
  // all nodes except non-visible textnodes and breaks.
  if (aArrayOfContents.IsEmpty()) {
    return true;
  }
  for (const OwningNonNull<nsIContent>& content : aArrayOfContents) {
    if (content->IsText()) {
      if (aHTMLEditor.IsInVisibleTextFrames(*content->AsText())) {
        return false;
      }
      continue;
    }
    // XXX If it's an element node, we should check whether it has visible
    //     frames or not.
    if (!content->IsElement() ||
        aHTMLEditor.IsEmptyNode(*content->AsElement())) {
      continue;
    }
    if (!content->IsHTMLElement(nsGkAtoms::br) ||
        aHTMLEditor.IsVisibleBRElement(content)) {
      return false;
    }
  }
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    DeleteTextAtStartAndEndOfRange(HTMLEditor& aHTMLEditor, nsRange& aRange) {
  EditorDOMPoint rangeStart(aRange.StartRef());
  EditorDOMPoint rangeEnd(aRange.EndRef());
  if (rangeStart.IsInTextNode() && !rangeStart.IsEndOfContainer()) {
    // Delete to last character
    OwningNonNull<Text> textNode = *rangeStart.GetContainerAsText();
    nsresult rv = aHTMLEditor.DeleteTextWithTransaction(
        textNode, rangeStart.Offset(),
        rangeStart.GetContainer()->Length() - rangeStart.Offset());
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }
  }
  if (rangeEnd.IsInTextNode() && !rangeEnd.IsStartOfContainer()) {
    // Delete to first character
    OwningNonNull<Text> textNode = *rangeEnd.GetContainerAsText();
    nsresult rv =
        aHTMLEditor.DeleteTextWithTransaction(textNode, 0, rangeEnd.Offset());
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  for (OwningNonNull<nsRange>& range : aRangesToDelete.Ranges()) {
    Result<bool, nsresult> result =
        ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
            aHTMLEditor, range, aSelectionWasCollapsed);
    if (result.isErr()) {
      NS_WARNING(
          "AutoBlockElementsJoiner::"
          "ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure() "
          "failed");
      return result.unwrapErr();
    }
    if (!result.unwrap()) {
      return NS_OK;
    }
  }

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }

  if (!canJoinThem.unwrap()) {
    return NS_SUCCESS_DOM_NO_OPERATION;
  }

  if (!joiner.CanJoinBlocks()) {
    return NS_OK;
  }

  nsresult rv = joiner.ComputeRangesToDelete(aHTMLEditor, EditorDOMPoint(),
                                             aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete() "
      "failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    HandleDeleteNonCollapsedRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  // Otherwise, delete every nodes in all ranges, then, clean up something.
  EditActionResult result(NS_OK);
  while (true) {
    AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                        &aRangesToDelete.FirstRangeRef());

    bool joinInclusiveAncestorBlockElements = true;
    for (auto& range : aRangesToDelete.Ranges()) {
      Result<bool, nsresult> deleteResult =
          DeleteNodesEntirelyInRangeButKeepTableStructure(
              aHTMLEditor, MOZ_KnownLive(range), aSelectionWasCollapsed);
      if (deleteResult.isErr()) {
        NS_WARNING(
            "AutoBlockElementsJoiner::"
            "DeleteNodesEntirelyInRangeButKeepTableStructure() failed");
        return EditActionResult(deleteResult.unwrapErr());
      }
      // XXX Completely odd.  Why don't we join blocks around each range?
      joinInclusiveAncestorBlockElements &= deleteResult.unwrap();
    }

    // Check endpoints for possible text deletion.  We can assume that if
    // text node is found, we can delete to end or to begining as
    // appropriate, since the case where both sel endpoints in same text
    // node was already handled (we wouldn't be here)
    nsresult rv = DeleteTextAtStartAndEndOfRange(
        aHTMLEditor, MOZ_KnownLive(aRangesToDelete.FirstRangeRef()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoBlockElementsJoiner::DeleteTextAtStartAndEndOfRange() failed");
      return EditActionHandled(rv);
    }

    if (!joinInclusiveAncestorBlockElements) {
      break;
    }

    AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                    *mRightContent);
    Result<bool, nsresult> canJoinThem = joiner.Prepare(aHTMLEditor);
    if (canJoinThem.isErr()) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
      return EditActionResult(canJoinThem.unwrapErr());
    }

    // If we're joining blocks: if deleting forward the selection should
    // be collapsed to the end of the selection, if deleting backward the
    // selection should be collapsed to the beginning of the selection.
    // But if we're not joining then the selection should collapse to the
    // beginning of the selection if we'redeleting forward, because the
    // end of the selection will still be in the next block. And same
    // thing for deleting backwards (selection should collapse to the end,
    // because the beginning will still be in the first block). See Bug
    // 507936.
    if (aDirectionAndAmount == nsIEditor::eNext) {
      aDirectionAndAmount = nsIEditor::ePrevious;
    } else {
      aDirectionAndAmount = nsIEditor::eNext;
    }

    if (!canJoinThem.inspect()) {
      result.MarkAsCanceled();
      break;
    }

    if (!joiner.CanJoinBlocks()) {
      break;
    }

    result |= joiner.Run(aHTMLEditor);
    if (result.Failed()) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
      return result;
    }
#ifdef DEBUG
    if (joiner.ShouldDeleteLeafContentInstead()) {
      NS_ASSERTION(result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning ignored, but returned not ignored");
    } else {
      NS_ASSERTION(!result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning handled, but returned ignored");
    }
#endif  // #ifdef DEBUG
    break;
  }

  nsresult rv =
      mDeleteRangesHandler->DeleteUnnecessaryNodesAndCollapseSelection(
          aHTMLEditor, aDirectionAndAmount,
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection() "
        "failed");
    return EditActionResult(rv);
  }

  return result.MarkAsHandled();
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    const EditorDOMPoint& aSelectionStartPoint,
    const EditorDOMPoint& aSelectionEndPoint) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());

  EditorDOMPoint atCaret(aSelectionStartPoint);
  EditorDOMPoint selectionEndPoint(aSelectionEndPoint);

  // If we're handling D&D, this is called to delete dragging item from the
  // tree.  In this case, we should remove parent blocks if it becomes empty.
  if (aHTMLEditor.GetEditAction() == EditAction::eDrop ||
      aHTMLEditor.GetEditAction() == EditAction::eDeleteByDrag) {
    MOZ_ASSERT((atCaret.GetContainer() == selectionEndPoint.GetContainer() &&
                atCaret.Offset() == selectionEndPoint.Offset()) ||
               (atCaret.GetContainer()->GetNextSibling() ==
                    selectionEndPoint.GetContainer() &&
                atCaret.IsEndOfContainer() &&
                selectionEndPoint.IsStartOfContainer()));
    {
      AutoTrackDOMPoint startTracker(aHTMLEditor.RangeUpdaterRef(), &atCaret);
      AutoTrackDOMPoint endTracker(aHTMLEditor.RangeUpdaterRef(),
                                   &selectionEndPoint);

      nsresult rv =
          DeleteParentBlocksWithTransactionIfEmpty(aHTMLEditor, atCaret);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::DeleteParentBlocksWithTransactionIfEmpty() failed");
        return rv;
      }
      aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks =
          rv == NS_OK;
    }
    // If we removed parent blocks, Selection should be collapsed at where
    // the most ancestor empty block has been.
    if (aHTMLEditor.TopLevelEditSubActionDataRef()
            .mDidDeleteEmptyParentBlocks) {
      nsresult rv = aHTMLEditor.CollapseSelectionTo(atCaret);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::CollapseSelectionTo() failed");
      return rv;
    }
  }

  if (NS_WARN_IF(!atCaret.IsInContentNode()) ||
      NS_WARN_IF(!selectionEndPoint.IsInContentNode())) {
    return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
  }

  // We might have left only collapsed white-space in the start/end nodes
  {
    AutoTrackDOMPoint startTracker(aHTMLEditor.RangeUpdaterRef(), &atCaret);
    AutoTrackDOMPoint endTracker(aHTMLEditor.RangeUpdaterRef(),
                                 &selectionEndPoint);

    nsresult rv = DeleteNodeIfInvisibleAndEditableTextNode(
        aHTMLEditor, MOZ_KnownLive(*atCaret.ContainerAsContent()));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
        "failed to remove start node, but ignored");
    rv = DeleteNodeIfInvisibleAndEditableTextNode(
        aHTMLEditor, MOZ_KnownLive(*selectionEndPoint.ContainerAsContent()));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
        "failed to remove end node, but ignored");
  }

  nsresult rv = aHTMLEditor.CollapseSelectionTo(
      aDirectionAndAmount == nsIEditor::ePrevious ? selectionEndPoint
                                                  : atCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode(
    HTMLEditor& aHTMLEditor, nsIContent& aContent) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  Text* text = aContent.GetAsText();
  if (!text) {
    return NS_OK;
  }

  if (aHTMLEditor.IsVisibleTextNode(*text) ||
      !HTMLEditUtils::IsSimplyEditableNode(*text)) {
    return NS_OK;
  }

  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(aContent);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::DeleteNodeWithTransaction() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteParentBlocksWithTransactionIfEmpty(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPoint) {
  MOZ_ASSERT(aPoint.IsSet());
  MOZ_ASSERT(aHTMLEditor.mPlaceholderBatch);

  // First, check there is visible contents before the point in current block.
  WSRunScanner wsScannerForPoint(aHTMLEditor, aPoint);
  if (!wsScannerForPoint.StartsFromCurrentBlockBoundary()) {
    // If there is visible node before the point, we shouldn't remove the
    // parent block.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }
  if (NS_WARN_IF(!wsScannerForPoint.GetStartReasonContent()) ||
      NS_WARN_IF(!wsScannerForPoint.GetStartReasonContent()->GetParentNode())) {
    return NS_ERROR_FAILURE;
  }
  if (wsScannerForPoint.GetEditingHost() ==
      wsScannerForPoint.GetStartReasonContent()) {
    // If we reach editing host, there is no parent blocks which can be removed.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }
  if (HTMLEditUtils::IsTableCellOrCaption(
          *wsScannerForPoint.GetStartReasonContent())) {
    // If we reach a <td>, <th> or <caption>, we shouldn't remove it even
    // becomes empty because removing such element changes the structure of
    // the <table>.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }

  // Next, check there is visible contents after the point in current block.
  WSScanResult forwardScanFromPointResult =
      wsScannerForPoint.ScanNextVisibleNodeOrBlockBoundaryFrom(aPoint);
  if (forwardScanFromPointResult.ReachedBRElement()) {
    // XXX In my understanding, this is odd.  The end reason may not be
    //     same as the reached <br> element because the equality is
    //     guaranteed only when ReachedCurrentBlockBoundary() returns true.
    //     However, looks like that this code assumes that
    //     GetEndReasonContent() returns the (or a) <br> element.
    NS_ASSERTION(wsScannerForPoint.GetEndReasonContent() ==
                     forwardScanFromPointResult.BRElementPtr(),
                 "End reason is not the reached <br> element");
    // If the <br> element is visible, we shouldn't remove the parent block.
    if (aHTMLEditor.IsVisibleBRElement(
            wsScannerForPoint.GetEndReasonContent())) {
      return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
    }
    if (wsScannerForPoint.GetEndReasonContent()->GetNextSibling()) {
      if (!WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
               aHTMLEditor, EditorRawDOMPoint::After(
                                *wsScannerForPoint.GetEndReasonContent()))
               .ReachedCurrentBlockBoundary()) {
        // If we couldn't reach the block's end after the invisible <br>,
        // that means that there is visible content.
        return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
      }
    }
  } else if (!forwardScanFromPointResult.ReachedCurrentBlockBoundary()) {
    // If we couldn't reach the block's end, the block has visible content.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }

  // Delete the parent block.
  EditorDOMPoint nextPoint(
      wsScannerForPoint.GetStartReasonContent()->GetParentNode(), 0);
  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(
      MOZ_KnownLive(*wsScannerForPoint.GetStartReasonContent()));
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
    return rv;
  }
  // If we reach editing host, return NS_OK.
  if (nextPoint.GetContainer() == wsScannerForPoint.GetEditingHost()) {
    return NS_OK;
  }

  // Otherwise, we need to check whether we're still in empty block or not.

  // If we have mutation event listeners, the next point is now outside of
  // editing host or editing hos has been changed.
  if (aHTMLEditor.MaybeHasMutationEventListeners(
          NS_EVENT_BITS_MUTATION_NODEREMOVED |
          NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
          NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED)) {
    Element* editingHost = aHTMLEditor.GetActiveEditingHost();
    if (NS_WARN_IF(!editingHost) ||
        NS_WARN_IF(editingHost != wsScannerForPoint.GetEditingHost())) {
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
    if (NS_WARN_IF(!EditorUtils::IsDescendantOf(*nextPoint.GetContainer(),
                                                *editingHost))) {
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
  }

  rv = DeleteParentBlocksWithTransactionIfEmpty(aHTMLEditor, nextPoint);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "DeleteParentBlocksWithTransactionIfEmpty() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteRangesWithTransaction(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  EditorBase::HowToHandleCollapsedRange howToHandleCollapsedRange =
      EditorBase::HowToHandleCollapsedRangeFor(aDirectionAndAmount);
  if (NS_WARN_IF(aRangesToDelete.IsCollapsed() &&
                 howToHandleCollapsedRange ==
                     EditorBase::HowToHandleCollapsedRange::Ignore)) {
    return NS_ERROR_FAILURE;
  }

  auto extendRangeToSelectCharacterForward =
      [](nsRange& aRange, const EditorRawDOMPointInText& aCaretPoint) -> void {
    const nsTextFragment& textFragment =
        aCaretPoint.ContainerAsText()->TextFragment();
    if (!textFragment.GetLength()) {
      return;
    }
    if (textFragment.IsHighSurrogateFollowedByLowSurrogateAt(
            aCaretPoint.Offset())) {
      DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
          aCaretPoint.ContainerAsText(), aCaretPoint.Offset(),
          aCaretPoint.ContainerAsText(), aCaretPoint.Offset() + 2);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "nsRange::SetStartAndEnd() failed");
      return;
    }
    DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
        aCaretPoint.ContainerAsText(), aCaretPoint.Offset(),
        aCaretPoint.ContainerAsText(), aCaretPoint.Offset() + 1);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                         "nsRange::SetStartAndEnd() failed");
  };
  auto extendRangeToSelectCharacterBackward =
      [](nsRange& aRange, const EditorRawDOMPointInText& aCaretPoint) -> void {
    if (aCaretPoint.IsStartOfContainer()) {
      return;
    }
    const nsTextFragment& textFragment =
        aCaretPoint.ContainerAsText()->TextFragment();
    if (!textFragment.GetLength()) {
      return;
    }
    if (textFragment.IsLowSurrogateFollowingHighSurrogateAt(
            aCaretPoint.Offset() - 1)) {
      DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
          aCaretPoint.ContainerAsText(), aCaretPoint.Offset() - 2,
          aCaretPoint.ContainerAsText(), aCaretPoint.Offset());
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "nsRange::SetStartAndEnd() failed");
      return;
    }
    DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
        aCaretPoint.ContainerAsText(), aCaretPoint.Offset() - 1,
        aCaretPoint.ContainerAsText(), aCaretPoint.Offset());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                         "nsRange::SetStartAndEnd() failed");
  };

  for (OwningNonNull<nsRange>& range : aRangesToDelete.Ranges()) {
    // If it's not collapsed, `DeleteRangeTransaction::Create()` will be called
    // with it and `DeleteRangeTransaction` won't modify the range.
    if (!range->Collapsed()) {
      continue;
    }

    if (howToHandleCollapsedRange ==
        EditorBase::HowToHandleCollapsedRange::Ignore) {
      continue;
    }

    // In the other cases, `EditorBase::CreateTransactionForCollapsedRange()`
    // will handle the collapsed range.
    EditorRawDOMPoint caretPoint(range->StartRef());
    if (howToHandleCollapsedRange ==
            EditorBase::HowToHandleCollapsedRange::ExtendBackward &&
        caretPoint.IsStartOfContainer()) {
      nsIContent* previousEditableContent =
          aHTMLEditor.GetPreviousEditableNode(*caretPoint.GetContainer());
      if (!previousEditableContent) {
        continue;
      }
      if (!previousEditableContent->IsText()) {
        IgnoredErrorResult ignoredError;
        range->SelectNode(*previousEditableContent, ignoredError);
        NS_WARNING_ASSERTION(!ignoredError.Failed(),
                             "nsRange::SelectNode() failed");
        continue;
      }

      extendRangeToSelectCharacterBackward(
          range,
          EditorRawDOMPointInText::AtEndOf(*previousEditableContent->AsText()));
      continue;
    }

    if (howToHandleCollapsedRange ==
            EditorBase::HowToHandleCollapsedRange::ExtendForward &&
        caretPoint.IsEndOfContainer()) {
      nsIContent* nextEditableContent =
          aHTMLEditor.GetNextEditableNode(*caretPoint.GetContainer());
      if (!nextEditableContent) {
        continue;
      }

      if (!nextEditableContent->IsText()) {
        IgnoredErrorResult ignoredError;
        range->SelectNode(*nextEditableContent, ignoredError);
        NS_WARNING_ASSERTION(!ignoredError.Failed(),
                             "nsRange::SelectNode() failed");
        continue;
      }

      extendRangeToSelectCharacterForward(
          range, EditorRawDOMPointInText(nextEditableContent->AsText(), 0));
      continue;
    }

    if (caretPoint.IsInTextNode()) {
      if (howToHandleCollapsedRange ==
          EditorBase::HowToHandleCollapsedRange::ExtendBackward) {
        extendRangeToSelectCharacterBackward(
            range, EditorRawDOMPointInText(caretPoint.ContainerAsText(),
                                           caretPoint.Offset()));
        continue;
      }
      extendRangeToSelectCharacterForward(
          range, EditorRawDOMPointInText(caretPoint.ContainerAsText(),
                                         caretPoint.Offset()));
      continue;
    }

    nsIContent* editableContent =
        howToHandleCollapsedRange ==
                EditorBase::HowToHandleCollapsedRange::ExtendBackward
            ? aHTMLEditor.GetPreviousEditableNode(caretPoint)
            : aHTMLEditor.GetNextEditableNode(caretPoint);
    if (!editableContent) {
      continue;
    }
    while (editableContent && editableContent->IsCharacterData() &&
           !editableContent->Length()) {
      editableContent =
          howToHandleCollapsedRange ==
                  EditorBase::HowToHandleCollapsedRange::ExtendBackward
              ? aHTMLEditor.GetPreviousEditableNode(*editableContent)
              : aHTMLEditor.GetNextEditableNode(*editableContent);
    }
    if (!editableContent) {
      continue;
    }

    if (!editableContent->IsText()) {
      IgnoredErrorResult ignoredError;
      range->SelectNode(*editableContent, ignoredError);
      NS_WARNING_ASSERTION(!ignoredError.Failed(),
                           "nsRange::SelectNode() failed");
      continue;
    }

    if (howToHandleCollapsedRange ==
        EditorBase::HowToHandleCollapsedRange::ExtendBackward) {
      extendRangeToSelectCharacterBackward(
          range, EditorRawDOMPointInText::AtEndOf(*editableContent->AsText()));
      continue;
    }
    extendRangeToSelectCharacterForward(
        range, EditorRawDOMPointInText(editableContent->AsText(), 0));
    continue;
  }

  return NS_OK;
}

template <typename EditorDOMPointType>
nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPointType& aStartPoint, const EditorDOMPointType& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes) {
  if (NS_WARN_IF(!aStartPoint.IsSet()) || NS_WARN_IF(!aEndPoint.IsSet())) {
    return NS_ERROR_INVALID_ARG;
  }

  // MOOSE: this routine needs to be modified to preserve the integrity of the
  // wsFragment info.

  if (aStartPoint == aEndPoint) {
    // Nothing to delete
    return NS_OK;
  }

  RefPtr<Element> editingHost = GetActiveEditingHost();
  auto deleteEmptyContentNodeWithTransaction =
      [this, &aTreatEmptyTextNodes, &editingHost](nsIContent& aContent)
          MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION -> nsresult {
    OwningNonNull<nsIContent> nodeToRemove = aContent;
    if (aTreatEmptyTextNodes ==
        TreatEmptyTextNodes::RemoveAllEmptyInlineAncestors) {
      Element* emptyParentElementToRemove =
          HTMLEditUtils::GetMostDistantAnscestorEditableEmptyInlineElement(
              nodeToRemove, editingHost);
      if (emptyParentElementToRemove) {
        nodeToRemove = *emptyParentElementToRemove;
      }
    }
    nsresult rv = DeleteNodeWithTransaction(nodeToRemove);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::DeleteNodeWithTransaction() failed");
    return rv;
  };

  if (aStartPoint.GetContainer() == aEndPoint.GetContainer() &&
      aStartPoint.IsInTextNode()) {
    if (aTreatEmptyTextNodes !=
            TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries &&
        aStartPoint.IsStartOfContainer() && aEndPoint.IsEndOfContainer()) {
      nsresult rv = deleteEmptyContentNodeWithTransaction(
          MOZ_KnownLive(*aStartPoint.ContainerAsText()));
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "deleteEmptyContentNodeWithTransaction() failed");
      return rv;
    }
    RefPtr<Text> textNode = aStartPoint.ContainerAsText();
    nsresult rv =
        DeleteTextWithTransaction(*textNode, aStartPoint.Offset(),
                                  aEndPoint.Offset() - aStartPoint.Offset());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::DeleteTextWithTransaction() failed");
    return rv;
  }

  RefPtr<nsRange> range =
      nsRange::Create(aStartPoint.ToRawRangeBoundary(),
                      aEndPoint.ToRawRangeBoundary(), IgnoreErrors());
  if (!range) {
    NS_WARNING("nsRange::Create() failed");
    return NS_ERROR_FAILURE;
  }

  // Collect editable text nodes in the given range.
  AutoTArray<OwningNonNull<Text>, 16> arrayOfTextNodes;
  DOMIterator iter;
  if (NS_FAILED(iter.Init(*range))) {
    return NS_OK;  // Nothing to delete in the range.
  }
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void*) {
        MOZ_ASSERT(aNode.IsText());
        return HTMLEditUtils::IsSimplyEditableNode(aNode);
      },
      arrayOfTextNodes);
  for (OwningNonNull<Text>& textNode : arrayOfTextNodes) {
    if (textNode == aStartPoint.GetContainer()) {
      if (aStartPoint.IsEndOfContainer()) {
        continue;
      }
      if (aStartPoint.IsStartOfContainer() &&
          aTreatEmptyTextNodes !=
              TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries) {
        nsresult rv = deleteEmptyContentNodeWithTransaction(
            MOZ_KnownLive(*aStartPoint.ContainerAsText()));
        if (NS_FAILED(rv)) {
          NS_WARNING("deleteEmptyContentNodeWithTransaction() failed");
          return rv;
        }
        continue;
      }
      nsresult rv = DeleteTextWithTransaction(
          MOZ_KnownLive(textNode), aStartPoint.Offset(),
          textNode->Length() - aStartPoint.Offset());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
        return rv;
      }
      continue;
    }

    if (textNode == aEndPoint.GetContainer()) {
      if (aEndPoint.IsStartOfContainer()) {
        break;
      }
      if (aEndPoint.IsEndOfContainer() &&
          aTreatEmptyTextNodes !=
              TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries) {
        nsresult rv = deleteEmptyContentNodeWithTransaction(
            MOZ_KnownLive(*aEndPoint.ContainerAsText()));
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "deleteEmptyContentNodeWithTransaction() failed");
        return rv;
      }
      nsresult rv = DeleteTextWithTransaction(MOZ_KnownLive(textNode), 0,
                                              aEndPoint.Offset());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }

    nsresult rv =
        deleteEmptyContentNodeWithTransaction(MOZ_KnownLive(textNode));
    if (NS_FAILED(rv)) {
      NS_WARNING("deleteEmptyContentNodeWithTransaction() failed");
      return rv;
    }
  }

  return NS_OK;
}

HTMLEditor::CharPointData
HTMLEditor::GetPreviousCharPointDataForNormalizingWhiteSpaces(
    const EditorDOMPointInText& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (!aPoint.IsStartOfContainer()) {
    return CharPointData::InSameTextNode(
        HTMLEditor::GetPreviousCharPointType(aPoint));
  }
  EditorDOMPointInText previousCharPoint =
      WSRunScanner::GetPreviousEditableCharPoint(*this, aPoint);
  if (!previousCharPoint.IsSet()) {
    return CharPointData::InDifferentTextNode(CharPointType::TextEnd);
  }
  return CharPointData::InDifferentTextNode(
      HTMLEditor::GetCharPointType(previousCharPoint));
}

HTMLEditor::CharPointData
HTMLEditor::GetInclusiveNextCharPointDataForNormalizingWhiteSpaces(
    const EditorDOMPointInText& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (!aPoint.IsEndOfContainer()) {
    return CharPointData::InSameTextNode(HTMLEditor::GetCharPointType(aPoint));
  }
  EditorDOMPointInText nextCharPoint =
      WSRunScanner::GetInclusiveNextEditableCharPoint(*this, aPoint);
  if (!nextCharPoint.IsSet()) {
    return CharPointData::InDifferentTextNode(CharPointType::TextEnd);
  }
  return CharPointData::InDifferentTextNode(
      HTMLEditor::GetCharPointType(nextCharPoint));
}

// static
void HTMLEditor::GenerateWhiteSpaceSequence(
    nsAString& aResult, uint32_t aLength,
    const CharPointData& aPreviousCharPointData,
    const CharPointData& aNextCharPointData) {
  MOZ_ASSERT(aResult.IsEmpty());
  MOZ_ASSERT(aLength);
  // For now, this method does not assume that result will be append to
  // white-space sequence in the text node.
  MOZ_ASSERT(aPreviousCharPointData.AcrossTextNodeBoundary() ||
             !aPreviousCharPointData.IsWhiteSpace());
  // For now, this method does not assume that the result will be inserted
  // into white-space sequence nor start of white-space sequence.
  MOZ_ASSERT(aNextCharPointData.AcrossTextNodeBoundary() ||
             !aNextCharPointData.IsWhiteSpace());

  if (aLength == 1) {
    // Even if previous/next char is in different text node, we should put
    // an ASCII white-space between visible characters.
    // XXX This means that this does not allow to put an NBSP in HTML editor
    //     without preformatted style.  However, Chrome has same issue too.
    if (aPreviousCharPointData.Type() == CharPointType::VisibleChar &&
        aNextCharPointData.Type() == CharPointType::VisibleChar) {
      aResult.Assign(HTMLEditUtils::kSpace);
      return;
    }
    // If it's start or end of text, put an NBSP.
    if (aPreviousCharPointData.Type() == CharPointType::TextEnd ||
        aNextCharPointData.Type() == CharPointType::TextEnd) {
      aResult.Assign(HTMLEditUtils::kNBSP);
      return;
    }
    // Now, the white-space will be inserted to a white-space sequence, but not
    // end of text.  We can put an ASCII white-space only when both sides are
    // not ASCII white-spaces.
    aResult.Assign(
        aPreviousCharPointData.Type() == CharPointType::ASCIIWhiteSpace ||
                aNextCharPointData.Type() == CharPointType::ASCIIWhiteSpace
            ? HTMLEditUtils::kNBSP
            : HTMLEditUtils::kSpace);
    return;
  }

  // Generate pairs of NBSP and ASCII white-space.
  aResult.SetLength(aLength);
  bool appendNBSP = true;  // Basically, starts with an NBSP.
  char16_t* lastChar = aResult.EndWriting() - 1;
  for (char16_t* iter = aResult.BeginWriting(); iter != lastChar; iter++) {
    *iter = appendNBSP ? HTMLEditUtils::kNBSP : HTMLEditUtils::kSpace;
    appendNBSP = !appendNBSP;
  }

  // If the final one is expected to an NBSP, we can put an NBSP simply.
  if (appendNBSP) {
    *lastChar = HTMLEditUtils::kNBSP;
    return;
  }

  // If next char point is end of text node or an ASCII white-space, we need to
  // put an NBSP.
  *lastChar =
      aNextCharPointData.AcrossTextNodeBoundary() ||
              aNextCharPointData.Type() == CharPointType::ASCIIWhiteSpace
          ? HTMLEditUtils::kNBSP
          : HTMLEditUtils::kSpace;
}

void HTMLEditor::ExtendRangeToDeleteWithNormalizingWhiteSpaces(
    EditorDOMPointInText& aStartToDelete, EditorDOMPointInText& aEndToDelete,
    nsAString& aNormalizedWhiteSpacesInStartNode,
    nsAString& aNormalizedWhiteSpacesInEndNode) const {
  MOZ_ASSERT(aStartToDelete.IsSetAndValid());
  MOZ_ASSERT(aEndToDelete.IsSetAndValid());
  MOZ_ASSERT(aStartToDelete.EqualsOrIsBefore(aEndToDelete));
  MOZ_ASSERT(aNormalizedWhiteSpacesInStartNode.IsEmpty());
  MOZ_ASSERT(aNormalizedWhiteSpacesInEndNode.IsEmpty());

  // First, check whether there is surrounding white-spaces or not, and if there
  // are, check whether they are collapsible or not.  Note that we shouldn't
  // touch white-spaces in different text nodes for performance, but we need
  // adjacent text node's first or last character information in some cases.
  EditorDOMPointInText precedingCharPoint =
      WSRunScanner::GetPreviousEditableCharPoint(*this, aStartToDelete);
  EditorDOMPointInText followingCharPoint =
      WSRunScanner::GetInclusiveNextEditableCharPoint(*this, aEndToDelete);
  // Blink-compat: Normalize white-spaces in first node only when not removing
  //               its last character or no text nodes follow the first node.
  //               If removing last character of first node and there are
  //               following text nodes, white-spaces in following text node are
  //               normalized instead.
  const bool removingLastCharOfStartNode =
      aStartToDelete.ContainerAsText() != aEndToDelete.ContainerAsText() ||
      (aEndToDelete.IsEndOfContainer() && followingCharPoint.IsSet());
  const bool maybeNormalizePrecedingWhiteSpaces =
      !removingLastCharOfStartNode && precedingCharPoint.IsSet() &&
      !precedingCharPoint.IsEndOfContainer() &&
      precedingCharPoint.ContainerAsText() ==
          aStartToDelete.ContainerAsText() &&
      precedingCharPoint.IsCharASCIISpaceOrNBSP() &&
      !EditorUtils::IsContentPreformatted(
          *precedingCharPoint.ContainerAsText());
  const bool maybeNormalizeFollowingWhiteSpaces =
      followingCharPoint.IsSet() && !followingCharPoint.IsEndOfContainer() &&
      (followingCharPoint.ContainerAsText() == aEndToDelete.ContainerAsText() ||
       removingLastCharOfStartNode) &&
      followingCharPoint.IsCharASCIISpaceOrNBSP() &&
      !EditorUtils::IsContentPreformatted(
          *followingCharPoint.ContainerAsText());

  if (!maybeNormalizePrecedingWhiteSpaces &&
      !maybeNormalizeFollowingWhiteSpaces) {
    return;  // There are no white-spaces.
  }

  // Next, consider the range to normalize.
  EditorDOMPointInText startToNormalize, endToNormalize;
  if (maybeNormalizePrecedingWhiteSpaces) {
    Maybe<uint32_t> previousCharOffsetOfWhiteSpaces =
        HTMLEditUtils::GetPreviousCharOffsetExceptWhiteSpaces(
            precedingCharPoint);
    startToNormalize.Set(precedingCharPoint.ContainerAsText(),
                         previousCharOffsetOfWhiteSpaces.isSome()
                             ? previousCharOffsetOfWhiteSpaces.value() + 1
                             : 0);
    MOZ_ASSERT(!startToNormalize.IsEndOfContainer());
  }
  if (maybeNormalizeFollowingWhiteSpaces) {
    Maybe<uint32_t> nextCharOffsetOfWhiteSpaces =
        HTMLEditUtils::GetInclusiveNextCharOffsetExceptWhiteSpaces(
            followingCharPoint);
    if (nextCharOffsetOfWhiteSpaces.isSome()) {
      endToNormalize.Set(followingCharPoint.ContainerAsText(),
                         nextCharOffsetOfWhiteSpaces.value());
    } else {
      endToNormalize.SetToEndOf(followingCharPoint.ContainerAsText());
    }
    MOZ_ASSERT(!endToNormalize.IsStartOfContainer());
  }

  // Next, retrieve surrounding information of white-space sequence.
  // If we're removing first text node's last character, we need to
  // normalize white-spaces starts from another text node.  In this case,
  // we need to lie for avoiding assertion in GenerateWhiteSpaceSequence().
  CharPointData previousCharPointData =
      removingLastCharOfStartNode
          ? CharPointData::InDifferentTextNode(CharPointType::TextEnd)
          : GetPreviousCharPointDataForNormalizingWhiteSpaces(
                startToNormalize.IsSet() ? startToNormalize : aStartToDelete);
  CharPointData nextCharPointData =
      GetInclusiveNextCharPointDataForNormalizingWhiteSpaces(
          endToNormalize.IsSet() ? endToNormalize : aEndToDelete);

  // Next, compute number of white-spaces in start/end node.
  uint32_t lengthInStartNode = 0, lengthInEndNode = 0;
  if (startToNormalize.IsSet()) {
    MOZ_ASSERT(startToNormalize.ContainerAsText() ==
               aStartToDelete.ContainerAsText());
    lengthInStartNode = aStartToDelete.Offset() - startToNormalize.Offset();
    MOZ_ASSERT(lengthInStartNode);
  }
  if (endToNormalize.IsSet()) {
    lengthInEndNode =
        endToNormalize.ContainerAsText() == aEndToDelete.ContainerAsText()
            ? endToNormalize.Offset() - aEndToDelete.Offset()
            : endToNormalize.Offset();
    MOZ_ASSERT(lengthInEndNode);
    // If we normalize white-spaces in a text node, we can replace all of them
    // with one ReplaceTextTransaction.
    if (endToNormalize.ContainerAsText() == aStartToDelete.ContainerAsText()) {
      lengthInStartNode += lengthInEndNode;
      lengthInEndNode = 0;
    }
  }

  MOZ_ASSERT(lengthInStartNode + lengthInEndNode);

  // Next, generate normalized white-spaces.
  if (!lengthInEndNode) {
    HTMLEditor::GenerateWhiteSpaceSequence(
        aNormalizedWhiteSpacesInStartNode, lengthInStartNode,
        previousCharPointData, nextCharPointData);
  } else if (!lengthInStartNode) {
    HTMLEditor::GenerateWhiteSpaceSequence(
        aNormalizedWhiteSpacesInEndNode, lengthInEndNode, previousCharPointData,
        nextCharPointData);
  } else {
    // For making `GenerateWhiteSpaceSequence()` simpler, we should create
    // whole white-space sequence first, then, copy to the out params.
    nsAutoString whiteSpaces;
    HTMLEditor::GenerateWhiteSpaceSequence(
        whiteSpaces, lengthInStartNode + lengthInEndNode, previousCharPointData,
        nextCharPointData);
    aNormalizedWhiteSpacesInStartNode =
        Substring(whiteSpaces, 0, lengthInStartNode);
    aNormalizedWhiteSpacesInEndNode = Substring(whiteSpaces, lengthInStartNode);
    MOZ_ASSERT(aNormalizedWhiteSpacesInEndNode.Length() == lengthInEndNode);
  }

  // TODO: Shrink the replacing range and string as far as possible because
  //       this may run a lot, i.e., HTMLEditor creates ReplaceTextTransaction
  //       a lot for normalizing white-spaces.  Then, each transaction shouldn't
  //       have all white-spaces every time because once it's normalized, we
  //       don't need to normalize all of the sequence again, but currently
  //       we do.

  // Finally, extend the range.
  if (startToNormalize.IsSet()) {
    aStartToDelete = startToNormalize;
  }
  if (endToNormalize.IsSet()) {
    aEndToDelete = endToNormalize;
  }
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces(
    const EditorDOMPointInText& aStartToDelete,
    const EditorDOMPointInText& aEndToDelete,
    TreatEmptyTextNodes aTreatEmptyTextNodes,
    DeleteDirection aDeleteDirection) {
  MOZ_ASSERT(aStartToDelete.IsSetAndValid());
  MOZ_ASSERT(aEndToDelete.IsSetAndValid());
  MOZ_ASSERT(aStartToDelete.EqualsOrIsBefore(aEndToDelete));

  // Use nsString for these replacing string because we should avoid to copy
  // the buffer from auto storange to ReplaceTextTransaction.
  nsString normalizedWhiteSpacesInFirstNode, normalizedWhiteSpacesInLastNode;

  // First, check whether we need to normalize white-spaces after deleting
  // the given range.
  EditorDOMPointInText startToDelete(aStartToDelete);
  EditorDOMPointInText endToDelete(aEndToDelete);
  ExtendRangeToDeleteWithNormalizingWhiteSpaces(
      startToDelete, endToDelete, normalizedWhiteSpacesInFirstNode,
      normalizedWhiteSpacesInLastNode);

  // If extended range is still collapsed, i.e., the caller just wants to
  // normalize white-space sequence, but there is no white-spaces which need to
  // be replaced, we need to do nothing here.
  if (startToDelete == endToDelete) {
    return EditorDOMPoint(aStartToDelete);
  }

  // Note that the container text node of startToDelete may be removed from
  // the tree if it becomes empty.  Therefore, we need to track the point.
  EditorDOMPoint newCaretPosition;
  if (aStartToDelete.ContainerAsText() == aEndToDelete.ContainerAsText()) {
    newCaretPosition = aEndToDelete;
  } else if (aDeleteDirection == DeleteDirection::Forward) {
    newCaretPosition.SetToEndOf(aStartToDelete.ContainerAsText());
  } else {
    newCaretPosition.Set(aEndToDelete.ContainerAsText(), 0);
  }

  // Then, modify the text nodes in the range.
  while (true) {
    AutoTrackDOMPoint trackingNewCaretPosition(RangeUpdaterRef(),
                                               &newCaretPosition);
    // Use ReplaceTextTransaction if we need to normalize white-spaces in
    // the first text node.
    if (!normalizedWhiteSpacesInFirstNode.IsEmpty()) {
      EditorDOMPoint trackingEndToDelete(endToDelete.ContainerAsText(),
                                         endToDelete.Offset());
      {
        AutoTrackDOMPoint trackEndToDelete(RangeUpdaterRef(),
                                           &trackingEndToDelete);
        uint32_t lengthToReplaceInFirstTextNode =
            startToDelete.ContainerAsText() ==
                    trackingEndToDelete.ContainerAsText()
                ? trackingEndToDelete.Offset() - startToDelete.Offset()
                : startToDelete.ContainerAsText()->TextLength() -
                      startToDelete.Offset();
        nsresult rv = ReplaceTextWithTransaction(
            MOZ_KnownLive(*startToDelete.ContainerAsText()),
            startToDelete.Offset(), lengthToReplaceInFirstTextNode,
            normalizedWhiteSpacesInFirstNode);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
          return Err(rv);
        }
        if (startToDelete.ContainerAsText() ==
            trackingEndToDelete.ContainerAsText()) {
          MOZ_ASSERT(normalizedWhiteSpacesInLastNode.IsEmpty());
          break;  // There is no more text which we need to delete.
        }
      }
      if (MaybeHasMutationEventListeners(
              NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
          (NS_WARN_IF(!trackingEndToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!trackingEndToDelete.IsInTextNode()))) {
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      MOZ_ASSERT(trackingEndToDelete.IsInTextNode());
      endToDelete.Set(trackingEndToDelete.ContainerAsText(),
                      trackingEndToDelete.Offset());
      // If the remaining range was modified by mutation event listener,
      // we should stop handling the deletion.
      startToDelete =
          EditorDOMPointInText::AtEndOf(*startToDelete.ContainerAsText());
      if (MaybeHasMutationEventListeners(
              NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
          NS_WARN_IF(!startToDelete.IsBefore(endToDelete))) {
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
    }
    // Delete ASCII whiteSpaces in the range simpley if there are some text
    // nodes which we don't need to replace their text.
    if (normalizedWhiteSpacesInLastNode.IsEmpty() ||
        startToDelete.ContainerAsText() != endToDelete.ContainerAsText()) {
      // If we need to replace text in the last text node, we should
      // delete text before its previous text node.
      EditorDOMPointInText endToDeleteExceptReplaceRange =
          normalizedWhiteSpacesInLastNode.IsEmpty()
              ? endToDelete
              : EditorDOMPointInText(endToDelete.ContainerAsText(), 0);
      if (startToDelete != endToDeleteExceptReplaceRange) {
        nsresult rv = DeleteTextAndTextNodesWithTransaction(
            startToDelete, endToDeleteExceptReplaceRange, aTreatEmptyTextNodes);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return Err(rv);
        }
        if (normalizedWhiteSpacesInLastNode.IsEmpty()) {
          break;  // There is no more text which we need to delete.
        }
        if (MaybeHasMutationEventListeners(
                NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED |
                NS_EVENT_BITS_MUTATION_NODEREMOVED |
                NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
                NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED) &&
            (NS_WARN_IF(!endToDeleteExceptReplaceRange.IsSetAndValid()) ||
             NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
             NS_WARN_IF(endToDelete.IsStartOfContainer()))) {
          return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
        }
        // Then, replace the text in the last text node.
        startToDelete = endToDeleteExceptReplaceRange;
      }
    }

    // Replace ASCII whiteSpaces in the range and following character in the
    // last text node.
    MOZ_ASSERT(!normalizedWhiteSpacesInLastNode.IsEmpty());
    MOZ_ASSERT(startToDelete.ContainerAsText() ==
               endToDelete.ContainerAsText());
    nsresult rv = ReplaceTextWithTransaction(
        MOZ_KnownLive(*startToDelete.ContainerAsText()), startToDelete.Offset(),
        endToDelete.Offset() - startToDelete.Offset(),
        normalizedWhiteSpacesInLastNode);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
      return Err(rv);
    }
    break;
  }

  if (!newCaretPosition.IsSetAndValid() ||
      !newCaretPosition.GetContainer()->IsInComposedDoc()) {
    NS_WARNING(
        "HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces() got lost "
        "the modifying line");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  // Look for leaf node to put caret if we remove some empty inline ancestors
  // at new caret position.
  if (!newCaretPosition.IsInTextNode()) {
    if (nsIContent* currentBlock = HTMLEditUtils::
            GetInclusiveAncestorEditableBlockElementOrInlineEditingHost(
                *newCaretPosition.ContainerAsContent())) {
      Element* editingHost = GetActiveEditingHost();
      // Try to put caret next to immediately after previous editable leaf.
      nsIContent* previousContent =
          HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
              newCaretPosition, *currentBlock, editingHost);
      if (previousContent && !HTMLEditUtils::IsBlockElement(*previousContent)) {
        newCaretPosition =
            previousContent->IsText() ||
                    HTMLEditUtils::IsContainerNode(*previousContent)
                ? EditorDOMPoint::AtEndOf(*previousContent)
                : EditorDOMPoint::After(*previousContent);
      }
      // But if the point is very first of a block element or immediately after
      // a child block, look for next editable leaf instead.
      else if (nsIContent* nextContent =
                   HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
                       newCaretPosition, *currentBlock, editingHost)) {
        newCaretPosition = nextContent->IsText() ||
                                   HTMLEditUtils::IsContainerNode(*nextContent)
                               ? EditorDOMPoint(nextContent, 0)
                               : EditorDOMPoint(nextContent);
      }
    }
  }

  // For compatibility with Blink, we should move caret to end of previous
  // text node if it's direct previous sibling of the first text node in the
  // range.
  if (newCaretPosition.IsStartOfContainer() &&
      newCaretPosition.IsInTextNode() &&
      newCaretPosition.GetContainer()->GetPreviousSibling() &&
      newCaretPosition.GetContainer()->GetPreviousSibling()->IsText()) {
    newCaretPosition.SetToEndOf(
        newCaretPosition.GetContainer()->GetPreviousSibling()->AsText());
  }

  {
    AutoTrackDOMPoint trackingNewCaretPosition(RangeUpdaterRef(),
                                               &newCaretPosition);
    nsresult rv = InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
        newCaretPosition);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::"
          "InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() failed");
      return Err(rv);
    }
  }
  if (!newCaretPosition.IsSetAndValid()) {
    NS_WARNING("Inserting <br> element caused unexpected DOM tree");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  return newCaretPosition;
}

nsresult HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
    const EditorDOMPoint& aPointToInsert) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToInsert.IsSet());

  if (!aPointToInsert.GetContainerAsContent()) {
    return NS_OK;
  }

  // If container of the point is not in a block, we don't need to put a
  // `<br>` element here.
  if (!HTMLEditUtils::IsBlockElement(*aPointToInsert.ContainerAsContent())) {
    return NS_OK;
  }

  WSRunScanner wsRunScanner(*this, aPointToInsert);
  // If the point is not start of a hard line, we don't need to put a `<br>`
  // element here.
  if (!wsRunScanner.StartsFromHardLineBreak()) {
    return NS_OK;
  }
  // If the point is not end of a hard line or the hard line does not end with
  // block boundary, we don't need to put a `<br>` element here.
  if (!wsRunScanner.EndsByBlockBoundary()) {
    return NS_OK;
  }

  // If we cannot insert a `<br>` element here, do nothing.
  if (!HTMLEditUtils::CanNodeContain(*aPointToInsert.GetContainer(),
                                     *nsGkAtoms::br)) {
    return NS_OK;
  }

  RefPtr<Element> brElement =
      InsertBRElementWithTransaction(aPointToInsert, nsIEditor::ePrevious);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(brElement,
                       "HTMLEditor::InsertBRElementWithTransaction() failed");
  return brElement ? NS_OK : NS_ERROR_FAILURE;
}

EditorDOMPoint HTMLEditor::GetGoodCaretPointFor(
    nsIContent& aContent, nsIEditor::EDirection aDirectionAndAmount) const {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aDirectionAndAmount == nsIEditor::eNext ||
             aDirectionAndAmount == nsIEditor::eNextWord ||
             aDirectionAndAmount == nsIEditor::ePrevious ||
             aDirectionAndAmount == nsIEditor::ePreviousWord ||
             aDirectionAndAmount == nsIEditor::eToBeginningOfLine ||
             aDirectionAndAmount == nsIEditor::eToEndOfLine);

  bool goingForward = (aDirectionAndAmount == nsIEditor::eNext ||
                       aDirectionAndAmount == nsIEditor::eNextWord ||
                       aDirectionAndAmount == nsIEditor::eToEndOfLine);

  // XXX Why don't we check whether the candidate position is enable or not?
  //     When the result is not editable point, caret will be enclosed in
  //     the non-editable content.

  // If we can put caret in aContent, return start or end in it.
  if (aContent.IsText() || HTMLEditUtils::IsContainerNode(aContent) ||
      NS_WARN_IF(!aContent.GetParentNode())) {
    return EditorDOMPoint(&aContent, goingForward ? 0 : aContent.Length());
  }

  // If we are going forward, put caret at aContent itself.
  if (goingForward) {
    return EditorDOMPoint(&aContent);
  }

  // If we are going backward, put caret to next node unless aContent is an
  // invisible `<br>` element.
  // XXX Shouldn't we put caret to first leaf of the next node?
  if (!aContent.IsHTMLElement(nsGkAtoms::br) || IsVisibleBRElement(&aContent)) {
    EditorDOMPoint ret(EditorDOMPoint::After(aContent));
    NS_WARNING_ASSERTION(ret.IsSet(), "Failed to set after aContent");
    return ret;
  }

  // Otherwise, we should put caret at the invisible `<br>` element.
  return EditorDOMPoint(&aContent);
}

Result<EditorDOMPoint, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::JoinNodesDeepWithTransaction(
        HTMLEditor& aHTMLEditor, nsIContent& aLeftContent,
        nsIContent& aRightContent) {
  // While the rightmost children and their descendants of the left node match
  // the leftmost children and their descendants of the right node, join them
  // up.

  nsCOMPtr<nsIContent> leftContentToJoin = &aLeftContent;
  nsCOMPtr<nsIContent> rightContentToJoin = &aRightContent;
  nsCOMPtr<nsINode> parentNode = aRightContent.GetParentNode();

  EditorDOMPoint ret;
  const HTMLEditUtils::StyleDifference kCompareStyle =
      aHTMLEditor.IsCSSEnabled() ? StyleDifference::CompareIfSpanElements
                                 : StyleDifference::Ignore;
  while (leftContentToJoin && rightContentToJoin && parentNode &&
         HTMLEditUtils::CanContentsBeJoined(
             *leftContentToJoin, *rightContentToJoin, kCompareStyle)) {
    uint32_t length = leftContentToJoin->Length();

    // Do the join
    nsresult rv = aHTMLEditor.JoinNodesWithTransaction(*leftContentToJoin,
                                                       *rightContentToJoin);
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::JoinNodesWithTransaction() failed");
      return Err(rv);
    }

    // XXX rightContentToJoin may have fewer children or shorter length text.
    //     So, we need some adjustment here.
    ret.Set(rightContentToJoin, length);
    if (NS_WARN_IF(!ret.IsSet())) {
      return Err(NS_ERROR_FAILURE);
    }

    if (parentNode->IsText()) {
      // We've joined all the way down to text nodes, we're done!
      return ret;
    }

    // Get new left and right nodes, and begin anew
    parentNode = rightContentToJoin;
    rightContentToJoin = parentNode->GetChildAt_Deprecated(length);
    if (rightContentToJoin) {
      leftContentToJoin = rightContentToJoin->GetPreviousSibling();
    } else {
      leftContentToJoin = nullptr;
    }

    // Skip over non-editable nodes
    while (leftContentToJoin && !EditorUtils::IsEditableContent(
                                    *leftContentToJoin, EditorType::HTML)) {
      leftContentToJoin = leftContentToJoin->GetPreviousSibling();
    }
    if (!leftContentToJoin) {
      return ret;
    }

    while (rightContentToJoin && !EditorUtils::IsEditableContent(
                                     *rightContentToJoin, EditorType::HTML)) {
      rightContentToJoin = rightContentToJoin->GetNextSibling();
    }
    if (!rightContentToJoin) {
      return ret;
    }
  }

  if (!ret.IsSet()) {
    NS_WARNING("HTMLEditor::JoinNodesDeepWithTransaction() joined no contents");
    return Err(NS_ERROR_FAILURE);
  }
  return ret;
}

Result<bool, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::AutoInclusiveAncestorBlockElementsJoiner::Prepare(
        const HTMLEditor& aHTMLEditor) {
  mLeftBlockElement =
      HTMLEditUtils::GetInclusiveAncestorBlockElementExceptHRElement(
          mInclusiveDescendantOfLeftBlockElement);
  mRightBlockElement =
      HTMLEditUtils::GetInclusiveAncestorBlockElementExceptHRElement(
          mInclusiveDescendantOfRightBlockElement);

  if (NS_WARN_IF(!IsSet())) {
    mCanJoinBlocks = false;
    return Err(NS_ERROR_UNEXPECTED);
  }

  // Don't join the blocks if both of them are basic structure of the HTML
  // document (Note that `<body>` can be joined with its children).
  if (mLeftBlockElement->IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                             nsGkAtoms::body) &&
      mRightBlockElement->IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                              nsGkAtoms::body)) {
    mCanJoinBlocks = false;
    return false;
  }

  if (HTMLEditUtils::IsAnyTableElement(mLeftBlockElement) ||
      HTMLEditUtils::IsAnyTableElement(mRightBlockElement)) {
    // Do not try to merge table elements, cancel the deletion.
    mCanJoinBlocks = false;
    return false;
  }

  // Bail if both blocks the same
  if (IsSameBlockElement()) {
    mCanJoinBlocks = true;  // XXX Anyway, Run() will ingore this case.
    mFallbackToDeleteLeafContent = true;
    return true;
  }

  // Joining a list item to its parent is a NOP.
  if (HTMLEditUtils::IsAnyListElement(mLeftBlockElement) &&
      HTMLEditUtils::IsListItem(mRightBlockElement) &&
      mRightBlockElement->GetParentNode() == mLeftBlockElement) {
    mCanJoinBlocks = false;
    return true;
  }

  // Special rule here: if we are trying to join list items, and they are in
  // different lists, join the lists instead.
  if (HTMLEditUtils::IsListItem(mLeftBlockElement) &&
      HTMLEditUtils::IsListItem(mRightBlockElement)) {
    // XXX leftListElement and/or rightListElement may be not list elements.
    Element* leftListElement = mLeftBlockElement->GetParentElement();
    Element* rightListElement = mRightBlockElement->GetParentElement();
    EditorDOMPoint atChildInBlock;
    if (leftListElement && rightListElement &&
        leftListElement != rightListElement &&
        !EditorUtils::IsDescendantOf(*leftListElement, *mRightBlockElement,
                                     &atChildInBlock) &&
        !EditorUtils::IsDescendantOf(*rightListElement, *mLeftBlockElement,
                                     &atChildInBlock)) {
      // There are some special complications if the lists are descendants of
      // the other lists' items.  Note that it is okay for them to be
      // descendants of the other lists themselves, which is the usual case for
      // sublists in our implementation.
      MOZ_DIAGNOSTIC_ASSERT(!atChildInBlock.IsSet());
      mLeftBlockElement = leftListElement;
      mRightBlockElement = rightListElement;
      mNewListElementTagNameOfRightListElement =
          Some(leftListElement->NodeInfo()->NameAtom());
    }
  }

  if (!EditorUtils::IsDescendantOf(*mLeftBlockElement, *mRightBlockElement,
                                   &mPointContainingTheOtherBlockElement)) {
    Unused << EditorUtils::IsDescendantOf(
        *mRightBlockElement, *mLeftBlockElement,
        &mPointContainingTheOtherBlockElement);
  }

  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mRightBlockElement) {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor, EditorDOMPoint::AtEndOf(mLeftBlockElement));
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`
    // returns ignored when:
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is nothing and
    // - There is no content to move from right block element.
    if (!mPrecedingInvisibleBRElement) {
      if (CanMergeLeftAndRightBlockElements()) {
        // Always marked as handled in this case.
        mFallbackToDeleteLeafContent = false;
      } else {
        // Marked as handled only when it actually moves a content node.
        Result<bool, nsresult> firstLineHasContent =
            aHTMLEditor.CanMoveOrDeleteSomethingInHardLine(
                mPointContainingTheOtherBlockElement.NextPoint());
        mFallbackToDeleteLeafContent =
            firstLineHasContent.isOk() && !firstLineHasContent.inspect();
      }
    } else {
      // Marked as handled when deleting the invisible `<br>` element.
      mFallbackToDeleteLeafContent = false;
    }
  } else if (mPointContainingTheOtherBlockElement.GetContainer() ==
             mLeftBlockElement) {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor, mPointContainingTheOtherBlockElement);
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()`
    // returns ignored when:
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is some and
    // - The right block element has no children
    // or,
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is nothing and
    // - There is no content to move from right block element.
    if (!mPrecedingInvisibleBRElement) {
      if (CanMergeLeftAndRightBlockElements()) {
        // Marked as handled only when it actualy moves a content node.
        Result<bool, nsresult> rightBlockHasContent =
            aHTMLEditor.CanMoveChildren(*mRightBlockElement,
                                        *mLeftBlockElement);
        mFallbackToDeleteLeafContent =
            rightBlockHasContent.isOk() && !rightBlockHasContent.inspect();
      } else {
        // Marked as handled only when it actually moves a content node.
        Result<bool, nsresult> firstLineHasContent =
            aHTMLEditor.CanMoveOrDeleteSomethingInHardLine(
                EditorRawDOMPoint(mRightBlockElement, 0));
        mFallbackToDeleteLeafContent =
            firstLineHasContent.isOk() && !firstLineHasContent.inspect();
      }
    } else {
      // Marked as handled when deleting the invisible `<br>` element.
      mFallbackToDeleteLeafContent = false;
    }
  } else {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor, EditorDOMPoint::AtEndOf(mLeftBlockElement));
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoLeftBlockElement()` always
    // return "handled".
    mFallbackToDeleteLeafContent = false;
  }

  mCanJoinBlocks = true;
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());
  MOZ_ASSERT(mLeftBlockElement);
  MOZ_ASSERT(mRightBlockElement);

  if (IsSameBlockElement()) {
    if (!aCaretPoint.IsSet()) {
      return NS_OK;  // The ranges are not collapsed, keep them as-is.
    }
    nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  EditorDOMPoint pointContainingTheOtherBlock;
  if (!EditorUtils::IsDescendantOf(*mLeftBlockElement, *mRightBlockElement,
                                   &pointContainingTheOtherBlock)) {
    Unused << EditorUtils::IsDescendantOf(
        *mRightBlockElement, *mLeftBlockElement, &pointContainingTheOtherBlock);
  }
  EditorDOMRange range =
      WSRunScanner::GetRangeForDeletingBlockElementBoundaries(
          aHTMLEditor, *mLeftBlockElement, *mRightBlockElement,
          pointContainingTheOtherBlock);
  if (!range.IsPositioned()) {
    NS_WARNING(
        "WSRunScanner::GetRangeForDeletingBlockElementBoundaries() failed");
    return NS_ERROR_FAILURE;
  }
  if (!aCaretPoint.IsSet()) {
    // Don't shrink the original range.
    bool noNeedToChangeStart = false;
    EditorDOMPoint atStart(aRangesToDelete.GetStartPointOfFirstRange());
    if (atStart.IsBefore(range.StartRef())) {
      // If the range starts from end of a container, and computed block
      // boundaries range starts from an invisible `<br>` element,  we
      // may need to shrink the range.
      nsIContent* nextContent =
          atStart.IsEndOfContainer() && range.StartRef().GetChild() &&
                  range.StartRef().GetChild()->IsHTMLElement(nsGkAtoms::br) &&
                  !aHTMLEditor.IsVisibleBRElement(range.StartRef().GetChild())
              ? aHTMLEditor.GetNextElementOrTextInBlock(
                    *atStart.ContainerAsContent())
              : nullptr;
      if (!nextContent || nextContent != range.StartRef().GetChild()) {
        noNeedToChangeStart = true;
        range.SetStart(aRangesToDelete.GetStartPointOfFirstRange());
      }
    }
    if (range.EndRef().IsBefore(aRangesToDelete.GetEndPointOfFirstRange())) {
      if (noNeedToChangeStart) {
        return NS_OK;  // We don't need to modify the range.
      }
      range.SetEnd(aRangesToDelete.GetEndPointOfFirstRange());
    }
  }
  // XXX Oddly, we join blocks only at the first range.
  nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
      range.StartRef().ToRawRangeBoundary(),
      range.EndRef().ToRawRangeBoundary());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoRangeArray::SetStartAndEnd() failed");
  return rv;
}

EditActionResult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    AutoInclusiveAncestorBlockElementsJoiner::Run(HTMLEditor& aHTMLEditor) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(mLeftBlockElement);
  MOZ_ASSERT(mRightBlockElement);

  if (IsSameBlockElement()) {
    return EditActionIgnored();
  }

  if (!mCanJoinBlocks) {
    return EditActionHandled();
  }

  // If the left block element is in the right block element, move the hard
  // line including the right block element to end of the left block.
  // However, if we are merging list elements, we don't join them.
  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mRightBlockElement) {
    EditActionResult result = WhiteSpaceVisibilityKeeper::
        MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement(
            aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
            MOZ_KnownLive(*mRightBlockElement),
            mPointContainingTheOtherBlockElement,
            mNewListElementTagNameOfRightListElement,
            MOZ_KnownLive(mPrecedingInvisibleBRElement));
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "WhiteSpaceVisibilityKeeper::"
                         "MergeFirstLineOfRightBlockElementIntoDescendantLeftBl"
                         "ockElement() failed");
    return result;
  }

  // If the right block element is in the left block element:
  // - move list item elements in the right block element to where the left
  //   list element is
  // - or first hard line in the right block element to where:
  //   - the left block element is.
  //   - or the given left content in the left block is.
  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mLeftBlockElement) {
    EditActionResult result = WhiteSpaceVisibilityKeeper::
        MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement(
            aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
            MOZ_KnownLive(*mRightBlockElement),
            mPointContainingTheOtherBlockElement,
            MOZ_KnownLive(*mInclusiveDescendantOfLeftBlockElement),
            mNewListElementTagNameOfRightListElement,
            MOZ_KnownLive(mPrecedingInvisibleBRElement));
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "WhiteSpaceVisibilityKeeper::"
                         "MergeFirstLineOfRightBlockElementIntoAncestorLeftBloc"
                         "kElement() failed");
    return result;
  }

  MOZ_ASSERT(!mPointContainingTheOtherBlockElement.IsSet());

  // Normal case.  Blocks are siblings, or at least close enough.  An example
  // of the latter is <p>paragraph</p><ul><li>one<li>two<li>three</ul>.  The
  // first li and the p are not true siblings, but we still want to join them
  // if you backspace from li into p.
  EditActionResult result = WhiteSpaceVisibilityKeeper::
      MergeFirstLineOfRightBlockElementIntoLeftBlockElement(
          aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
          MOZ_KnownLive(*mRightBlockElement),
          mNewListElementTagNameOfRightListElement,
          MOZ_KnownLive(mPrecedingInvisibleBRElement));
  NS_WARNING_ASSERTION(
      result.Succeeded(),
      "WhiteSpaceVisibilityKeeper::"
      "MergeFirstLineOfRightBlockElementIntoLeftBlockElement() failed");
  return result;
}

template <typename PT, typename CT>
Result<bool, nsresult> HTMLEditor::CanMoveOrDeleteSomethingInHardLine(
    const EditorDOMPointBase<PT, CT>& aPointInHardLine) const {
  RefPtr<nsRange> oneLineRange = CreateRangeExtendedToHardLineStartAndEnd(
      aPointInHardLine.ToRawRangeBoundary(),
      aPointInHardLine.ToRawRangeBoundary(),
      EditSubAction::eMergeBlockContents);
  if (!oneLineRange || oneLineRange->Collapsed() ||
      !oneLineRange->IsPositioned() ||
      !oneLineRange->GetStartContainer()->IsContent() ||
      !oneLineRange->GetEndContainer()->IsContent()) {
    return false;
  }

  // If there is only a padding `<br>` element in a empty block, it's selected
  // by `SelectBRElementIfCollapsedInEmptyBlock()`.  However, it won't be
  // moved.  Although it'll be deleted, `MoveOneHardLineContents()` returns
  // "ignored".  Therefore, we should return `false` in this case.
  if (nsIContent* childContent = oneLineRange->GetChildAtStartOffset()) {
    if (childContent->IsHTMLElement(nsGkAtoms::br) &&
        childContent->GetParent()) {
      if (Element* blockElement =
              HTMLEditUtils::GetInclusiveAncestorBlockElement(
                  *childContent->GetParent())) {
        if (IsEmptyNode(*blockElement, true, false)) {
          return false;
        }
      }
    }
  }

  nsINode* commonAncestor = oneLineRange->GetClosestCommonInclusiveAncestor();
  // Currently, we move non-editable content nodes too.
  EditorRawDOMPoint startPoint(oneLineRange->StartRef());
  if (!startPoint.IsEndOfContainer()) {
    return true;
  }
  EditorRawDOMPoint endPoint(oneLineRange->EndRef());
  if (!endPoint.IsStartOfContainer()) {
    return true;
  }
  if (startPoint.GetContainer() != commonAncestor) {
    while (true) {
      EditorRawDOMPoint pointInParent(startPoint.GetContainerAsContent());
      if (NS_WARN_IF(!pointInParent.IsInContentNode())) {
        return Err(NS_ERROR_FAILURE);
      }
      if (pointInParent.GetContainer() == commonAncestor) {
        startPoint = pointInParent;
        break;
      }
      if (!pointInParent.IsEndOfContainer()) {
        return true;
      }
    }
  }
  if (endPoint.GetContainer() != commonAncestor) {
    while (true) {
      EditorRawDOMPoint pointInParent(endPoint.GetContainerAsContent());
      if (NS_WARN_IF(!pointInParent.IsInContentNode())) {
        return Err(NS_ERROR_FAILURE);
      }
      if (pointInParent.GetContainer() == commonAncestor) {
        endPoint = pointInParent;
        break;
      }
      if (!pointInParent.IsStartOfContainer()) {
        return true;
      }
    }
  }
  // If start point and end point in the common ancestor are direct siblings,
  // there is no content to move or delete.
  // E.g., `<b>abc<br>[</b><i>]<br>def</i>`.
  return startPoint.GetNextSiblingOfChild() != endPoint.GetChild();
}

MoveNodeResult HTMLEditor::MoveOneHardLineContents(
    const EditorDOMPoint& aPointInHardLine,
    const EditorDOMPoint& aPointToInsert,
    MoveToEndOfContainer
        aMoveToEndOfContainer /* = MoveToEndOfContainer::No */) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodesInOneHardLine(
      aPointInHardLine, arrayOfContents, EditSubAction::eMergeBlockContents,
      HTMLEditor::CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::SplitInlinesAndCollectEditTargetNodesInOneHardLine("
        "eMergeBlockContents, CollectNonEditableNodes::Yes) failed");
    return MoveNodeResult(rv);
  }
  if (arrayOfContents.IsEmpty()) {
    return MoveNodeIgnored(aPointToInsert);
  }

  uint32_t offset = aPointToInsert.Offset();
  MoveNodeResult result;
  for (auto& content : arrayOfContents) {
    if (aMoveToEndOfContainer == MoveToEndOfContainer::Yes) {
      // For backward compatibility, we should move contents to end of the
      // container if this is called with MoveToEndOfContainer::Yes.
      offset = aPointToInsert.GetContainer()->Length();
    }
    // get the node to act on
    if (HTMLEditUtils::IsBlockElement(content)) {
      // For block nodes, move their contents only, then delete block.
      result |= MoveChildrenWithTransaction(
          MOZ_KnownLive(*content->AsElement()),
          EditorDOMPoint(aPointToInsert.GetContainer(), offset));
      if (result.Failed()) {
        NS_WARNING("HTMLEditor::MoveChildrenWithTransaction() failed");
        return result;
      }
      offset = result.NextInsertionPointRef().Offset();
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      DebugOnly<nsresult> rvIgnored =
          DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_WARN_IF(Destroyed())) {
        return MoveNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "HTMLEditor::DeleteNodeWithTransaction() failed, but ignored");
      result.MarkAsHandled();
      if (MaybeHasMutationEventListeners()) {
        // Mutation event listener may make `offset` value invalid with
        // removing some previous children while we call
        // `DeleteNodeWithTransaction()` so that we should adjust it here.
        offset = std::min(offset, aPointToInsert.GetContainer()->Length());
      }
      continue;
    }
    // XXX Different from the above block, we ignore error of moving nodes.
    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
    // keep it alive.
    MoveNodeResult moveNodeResult = MoveNodeOrChildrenWithTransaction(
        MOZ_KnownLive(content),
        EditorDOMPoint(aPointToInsert.GetContainer(), offset));
    if (NS_WARN_IF(moveNodeResult.EditorDestroyed())) {
      return MoveNodeResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        moveNodeResult.Succeeded(),
        "HTMLEditor::MoveNodeOrChildrenWithTransaction() failed, but ignored");
    if (moveNodeResult.Succeeded()) {
      offset = moveNodeResult.NextInsertionPointRef().Offset();
      result |= moveNodeResult;
    }
  }

  NS_WARNING_ASSERTION(
      result.Succeeded(),
      "Last HTMLEditor::MoveNodeOrChildrenWithTransaction() failed");
  return result;
}

Result<bool, nsresult> HTMLEditor::CanMoveNodeOrChildren(
    const nsIContent& aContent, const nsINode& aNewContainer) const {
  if (HTMLEditUtils::CanNodeContain(aNewContainer, aContent)) {
    return true;
  }
  if (aContent.IsElement()) {
    return CanMoveChildren(*aContent.AsElement(), aNewContainer);
  }
  return true;
}

MoveNodeResult HTMLEditor::MoveNodeOrChildrenWithTransaction(
    nsIContent& aContent, const EditorDOMPoint& aPointToInsert) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToInsert.IsSet());

  // Check if this node can go into the destination node
  if (HTMLEditUtils::CanNodeContain(*aPointToInsert.GetContainer(), aContent)) {
    // If it can, move it there.
    uint32_t offsetAtInserting = aPointToInsert.Offset();
    nsresult rv = MoveNodeWithTransaction(aContent, aPointToInsert);
    if (NS_WARN_IF(Destroyed())) {
      return MoveNodeResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return MoveNodeResult(rv);
    }
    // Advance DOM point with offset for keeping backward compatibility.
    // XXX Where should we insert next content if a mutation event listener
    //     break the relation of offset and moved node?
    return MoveNodeHandled(aPointToInsert.GetContainer(), ++offsetAtInserting);
  }

  // If it can't, move its children (if any), and then delete it.
  MoveNodeResult result;
  if (aContent.IsElement()) {
    result = MoveChildrenWithTransaction(MOZ_KnownLive(*aContent.AsElement()),
                                         aPointToInsert);
    if (result.Failed()) {
      NS_WARNING("HTMLEditor::MoveChildrenWithTransaction() failed");
      return result;
    }
  } else {
    result = MoveNodeHandled(aPointToInsert);
  }

  nsresult rv = DeleteNodeWithTransaction(aContent);
  if (NS_WARN_IF(Destroyed())) {
    return MoveNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
    return MoveNodeResult(rv);
  }
  if (MaybeHasMutationEventListeners()) {
    // Mutation event listener may make `offset` value invalid with
    // removing some previous children while we call
    // `DeleteNodeWithTransaction()` so that we should adjust it here.
    if (!result.NextInsertionPointRef().IsSetAndValid()) {
      result = MoveNodeHandled(
          EditorDOMPoint::AtEndOf(*aPointToInsert.GetContainer()));
    }
  }
  return result;
}

Result<bool, nsresult> HTMLEditor::CanMoveChildren(
    const Element& aElement, const nsINode& aNewContainer) const {
  if (NS_WARN_IF(&aElement == &aNewContainer)) {
    return Err(NS_ERROR_FAILURE);
  }
  for (nsIContent* childContent = aElement.GetFirstChild(); childContent;
       childContent = childContent->GetNextSibling()) {
    Result<bool, nsresult> result =
        CanMoveNodeOrChildren(*childContent, aNewContainer);
    if (result.isErr() || result.inspect()) {
      return result;
    }
  }
  return false;
}

MoveNodeResult HTMLEditor::MoveChildrenWithTransaction(
    Element& aElement, const EditorDOMPoint& aPointToInsert) {
  MOZ_ASSERT(aPointToInsert.IsSet());

  if (NS_WARN_IF(&aElement == aPointToInsert.GetContainer())) {
    return MoveNodeResult(NS_ERROR_INVALID_ARG);
  }

  MoveNodeResult result = MoveNodeIgnored(aPointToInsert);
  while (aElement.GetFirstChild()) {
    result |= MoveNodeOrChildrenWithTransaction(
        MOZ_KnownLive(*aElement.GetFirstChild()), result.NextInsertionPoint());
    if (result.Failed()) {
      NS_WARNING("HTMLEditor::MoveNodeOrChildrenWithTransaction() failed");
      return result;
    }
  }
  return result;
}

void HTMLEditor::MoveAllChildren(nsINode& aContainer,
                                 const EditorRawDOMPoint& aPointToInsert,
                                 ErrorResult& aError) {
  MOZ_ASSERT(!aError.Failed());

  if (!aContainer.HasChildren()) {
    return;
  }
  nsIContent* firstChild = aContainer.GetFirstChild();
  if (NS_WARN_IF(!firstChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  nsIContent* lastChild = aContainer.GetLastChild();
  if (NS_WARN_IF(!lastChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  MoveChildrenBetween(*firstChild, *lastChild, aPointToInsert, aError);
  NS_WARNING_ASSERTION(!aError.Failed(),
                       "HTMLEditor::MoveChildrenBetween() failed");
}

void HTMLEditor::MoveChildrenBetween(nsIContent& aFirstChild,
                                     nsIContent& aLastChild,
                                     const EditorRawDOMPoint& aPointToInsert,
                                     ErrorResult& aError) {
  nsCOMPtr<nsINode> oldContainer = aFirstChild.GetParentNode();
  if (NS_WARN_IF(oldContainer != aLastChild.GetParentNode()) ||
      NS_WARN_IF(!aPointToInsert.IsSet()) ||
      NS_WARN_IF(!aPointToInsert.CanContainerHaveChildren())) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }

  // First, store all children which should be moved to the new container.
  AutoTArray<nsCOMPtr<nsIContent>, 10> children;
  for (nsIContent* child = &aFirstChild; child;
       child = child->GetNextSibling()) {
    children.AppendElement(child);
    if (child == &aLastChild) {
      break;
    }
  }

  if (NS_WARN_IF(children.LastElement() != &aLastChild)) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }

  nsCOMPtr<nsINode> newContainer = aPointToInsert.GetContainer();
  nsCOMPtr<nsIContent> nextNode = aPointToInsert.GetChild();
  for (size_t i = children.Length(); i > 0; --i) {
    nsCOMPtr<nsIContent>& child = children[i - 1];
    if (child->GetParentNode() != oldContainer) {
      // If the child has been moved to different container, we shouldn't
      // touch it.
      continue;
    }
    oldContainer->RemoveChild(*child, aError);
    if (aError.Failed()) {
      NS_WARNING("nsINode::RemoveChild() failed");
      return;
    }
    if (nextNode) {
      // If we're not appending the children to the new container, we should
      // check if referring next node of insertion point is still in the new
      // container.
      EditorRawDOMPoint pointToInsert(nextNode);
      if (NS_WARN_IF(!pointToInsert.IsSet()) ||
          NS_WARN_IF(pointToInsert.GetContainer() != newContainer)) {
        // The next node of insertion point has been moved by mutation observer.
        // Let's stop moving the remaining nodes.
        // XXX Or should we move remaining children after the last moved child?
        aError.Throw(NS_ERROR_FAILURE);
        return;
      }
    }
    newContainer->InsertBefore(*child, nextNode, aError);
    if (aError.Failed()) {
      NS_WARNING("nsINode::InsertBefore() failed");
      return;
    }
    // If the child was inserted or appended properly, the following children
    // should be inserted before it.  Otherwise, keep using current position.
    if (child->GetParentNode() == newContainer) {
      nextNode = child;
    }
  }
}

void HTMLEditor::MovePreviousSiblings(nsIContent& aChild,
                                      const EditorRawDOMPoint& aPointToInsert,
                                      ErrorResult& aError) {
  MOZ_ASSERT(!aError.Failed());

  if (NS_WARN_IF(!aChild.GetParentNode())) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }
  nsIContent* firstChild = aChild.GetParentNode()->GetFirstChild();
  if (NS_WARN_IF(!firstChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  nsIContent* lastChild =
      &aChild == firstChild ? firstChild : aChild.GetPreviousSibling();
  if (NS_WARN_IF(!lastChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  MoveChildrenBetween(*firstChild, *lastChild, aPointToInsert, aError);
  NS_WARNING_ASSERTION(!aError.Failed(),
                       "HTMLEditor::MoveChildrenBetween() failed");
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    DeleteContentButKeepTableStructure(HTMLEditor& aHTMLEditor,
                                       nsIContent& aContent) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsAnyTableElementButNotTable(&aContent)) {
    nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(aContent);
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::DeleteNodeWithTransaction() failed");
    return rv;
  }

  // XXX For performance, this should just call
  //     DeleteContentButKeepTableStructure() while there are children in
  //     aContent.  If we need to avoid infinite loop because mutation event
  //     listeners can add unexpected nodes into aContent, we should just loop
  //     only original count of the children.
  AutoTArray<OwningNonNull<nsIContent>, 10> childList;
  for (nsIContent* child = aContent.GetFirstChild(); child;
       child = child->GetNextSibling()) {
    childList.AppendElement(*child);
  }

  for (const auto& child : childList) {
    // MOZ_KnownLive because 'childList' is guaranteed to
    // keep it alive.
    nsresult rv =
        DeleteContentButKeepTableStructure(aHTMLEditor, MOZ_KnownLive(child));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteContentButKeepTableStructure() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::DeleteMostAncestorMailCiteElementIfEmpty(
    nsIContent& aContent) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // The element must be `<blockquote type="cite">` or
  // `<span _moz_quote="true">`.
  RefPtr<Element> mailCiteElement = GetMostAncestorMailCiteElement(aContent);
  if (!mailCiteElement) {
    return NS_OK;
  }
  bool seenBR = false;
  if (!IsEmptyNodeImpl(*mailCiteElement, true, true, false, &seenBR)) {
    return NS_OK;
  }
  EditorDOMPoint atEmptyMailCiteElement(mailCiteElement);
  {
    AutoEditorDOMPointChildInvalidator lockOffset(atEmptyMailCiteElement);
    nsresult rv = DeleteNodeWithTransaction(*mailCiteElement);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  if (!atEmptyMailCiteElement.IsSet() || !seenBR) {
    NS_WARNING_ASSERTION(
        atEmptyMailCiteElement.IsSet(),
        "Mutation event listener might changed the DOM tree during "
        "HTMLEditor::DeleteNodeWithTransaction(), but ignored");
    return NS_OK;
  }

  RefPtr<Element> brElement =
      InsertBRElementWithTransaction(atEmptyMailCiteElement);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (!brElement) {
    NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv = CollapseSelectionTo(EditorRawDOMPoint(brElement));
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::::CollapseSelectionTo() failed, but ignored");
  return NS_OK;
}

EditActionResult HTMLEditor::MakeOrChangeListAndListItemAsSubAction(
    nsAtom& aListElementOrListItemElementTagName, const nsAString& aBulletType,
    SelectAllOfCurrentList aSelectAllOfCurrentList) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(&aListElementOrListItemElementTagName == nsGkAtoms::ul ||
             &aListElementOrListItemElementTagName == nsGkAtoms::ol ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dl ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dd ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dt);

  if (NS_WARN_IF(!mInitSucceeded)) {
    return EditActionIgnored(NS_ERROR_NOT_INITIALIZED);
  }

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);

  // XXX EditSubAction::eCreateOrChangeDefinitionListItem and
  //     EditSubAction::eCreateOrChangeList are treated differently in
  //     HTMLEditor::MaybeSplitElementsAtEveryBRElement().  Only when
  //     EditSubAction::eCreateOrChangeList, it splits inline nodes.
  //     Currently, it shouldn't be done when we called for formatting
  //     `<dd>` or `<dt>` by
  //     HTMLEditor::MakeDefinitionListItemWithTransaction().  But this
  //     difference may be a bug.  We should investigate this later.
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this,
      &aListElementOrListItemElementTagName == nsGkAtoms::dd ||
              &aListElementOrListItemElementTagName == nsGkAtoms::dt
          ? EditSubAction::eCreateOrChangeDefinitionListItem
          : EditSubAction::eCreateOrChangeList,
      nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  nsAtom* listTagName = nullptr;
  nsAtom* listItemTagName = nullptr;
  if (&aListElementOrListItemElementTagName == nsGkAtoms::ul ||
      &aListElementOrListItemElementTagName == nsGkAtoms::ol) {
    listTagName = &aListElementOrListItemElementTagName;
    listItemTagName = nsGkAtoms::li;
  } else if (&aListElementOrListItemElementTagName == nsGkAtoms::dl) {
    listTagName = &aListElementOrListItemElementTagName;
    listItemTagName = nsGkAtoms::dd;
  } else if (&aListElementOrListItemElementTagName == nsGkAtoms::dd ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dt) {
    listTagName = nsGkAtoms::dl;
    listItemTagName = &aListElementOrListItemElementTagName;
  } else {
    NS_WARNING(
        "aListElementOrListItemElementTagName was neither list element name "
        "nor "
        "definition listitem element name");
    return EditActionResult(NS_ERROR_INVALID_ARG);
  }

  // Expands selection range to include the immediate block parent, and then
  // further expands to include any ancestors whose children are all in the
  // range.
  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(rv);
    }
  }

  // ChangeSelectedHardLinesToList() creates AutoSelectionRestorer.
  // Therefore, even if it returns NS_OK, editor might have been destroyed
  // at restoring Selection.
  result = ChangeSelectedHardLinesToList(MOZ_KnownLive(*listTagName),
                                         MOZ_KnownLive(*listItemTagName),
                                         aBulletType, aSelectAllOfCurrentList);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(result.Succeeded(),
                       "HTMLEditor::ChangeSelectedHardLinesToList() failed");
  return result;
}

EditActionResult HTMLEditor::ChangeSelectedHardLinesToList(
    nsAtom& aListElementTagName, nsAtom& aListItemElementTagName,
    const nsAString& aBulletType,
    SelectAllOfCurrentList aSelectAllOfCurrentList) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  AutoSelectionRestorer restoreSelectionLater(*this);

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  Element* parentListElement =
      aSelectAllOfCurrentList == SelectAllOfCurrentList::Yes
          ? GetParentListElementAtSelection()
          : nullptr;
  if (parentListElement) {
    arrayOfContents.AppendElement(
        OwningNonNull<nsIContent>(*parentListElement));
  } else {
    AutoTransactionsConserveSelection dontChangeMySelection(*this);
    nsresult rv =
        SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
            arrayOfContents, EditSubAction::eCreateOrChangeList,
            CollectNonEditableNodes::No);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::"
          "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges("
          "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
      return EditActionResult(rv);
    }
  }

  // check if all our nodes are <br>s, or empty inlines
  bool bOnlyBreaks = true;
  for (auto& content : arrayOfContents) {
    // if content is not a Break or empty inline, we're done
    if (!content->IsHTMLElement(nsGkAtoms::br) && !IsEmptyInlineNode(content)) {
      bOnlyBreaks = false;
      break;
    }
  }

  // if no nodes, we make empty list.  Ditto if the user tried to make a list
  // of some # of breaks.
  if (arrayOfContents.IsEmpty() || bOnlyBreaks) {
    // if only breaks, delete them
    if (bOnlyBreaks) {
      for (auto& content : arrayOfContents) {
        // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
        // keep it alive.
        nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
          return EditActionResult(rv);
        }
      }
    }

    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionResult(NS_ERROR_FAILURE);
    }

    EditorDOMPoint atStartOfSelection(firstRange->StartRef());
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }

    // Make sure we can put a list here.
    if (!HTMLEditUtils::CanNodeContain(*atStartOfSelection.GetContainer(),
                                       aListElementTagName)) {
      return EditActionCanceled();
    }

    SplitNodeResult splitAtSelectionStartResult =
        MaybeSplitAncestorsForInsertWithTransaction(aListElementTagName,
                                                    atStartOfSelection);
    if (splitAtSelectionStartResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
      return EditActionResult(splitAtSelectionStartResult.Rv());
    }
    RefPtr<Element> theList = CreateNodeWithTransaction(
        aListElementTagName, splitAtSelectionStartResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (!theList) {
      NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
      return EditActionResult(NS_ERROR_FAILURE);
    }

    RefPtr<Element> theListItem = CreateNodeWithTransaction(
        aListItemElementTagName, EditorDOMPoint(theList, 0));
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (!theListItem) {
      NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
      return EditActionResult(NS_ERROR_FAILURE);
    }

    // remember our new block for postprocessing
    TopLevelEditSubActionDataRef().mNewBlockElement = theListItem;
    // Put selection in new list item and don't restore the Selection.
    restoreSelectionLater.Abort();
    nsresult rv = CollapseSelectionToStartOf(*theListItem);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    return EditActionResult(rv);
  }

  // if there is only one node in the array, and it is a list, div, or
  // blockquote, then look inside of it until we find inner list or content.
  if (arrayOfContents.Length() == 1) {
    if (Element* deepestDivBlockquoteOrListElement =
            GetDeepestEditableOnlyChildDivBlockquoteOrListElement(
                arrayOfContents[0])) {
      if (deepestDivBlockquoteOrListElement->IsAnyOfHTMLElements(
              nsGkAtoms::div, nsGkAtoms::blockquote)) {
        arrayOfContents.Clear();
        CollectChildren(*deepestDivBlockquoteOrListElement, arrayOfContents, 0,
                        CollectListChildren::No, CollectTableChildren::No,
                        CollectNonEditableNodes::Yes);
      } else {
        arrayOfContents.ReplaceElementAt(
            0, OwningNonNull<nsIContent>(*deepestDivBlockquoteOrListElement));
      }
    }
  }

  // Ok, now go through all the nodes and put then in the list,
  // or whatever is approriate.  Wohoo!

  uint32_t countOfCollectedContents = arrayOfContents.Length();
  RefPtr<Element> curList, prevListItem;

  for (uint32_t i = 0; i < countOfCollectedContents; i++) {
    // here's where we actually figure out what to do
    OwningNonNull<nsIContent> content = arrayOfContents[i];

    // make sure we don't assemble content that is in different table cells
    // into the same list.  respect table cell boundaries when listifying.
    if (curList &&
        HTMLEditor::NodesInDifferentTableElements(*curList, content)) {
      curList = nullptr;
    }

    // If current node is a `<br>` element, delete it and forget previous
    // list item element.
    // If current node is an empty inline node, just delete it.
    if (EditorUtils::IsEditableContent(content, EditorType::HTML) &&
        (content->IsHTMLElement(nsGkAtoms::br) || IsEmptyInlineNode(content))) {
      nsresult rv = DeleteNodeWithTransaction(*content);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return EditActionResult(rv);
      }
      if (content->IsHTMLElement(nsGkAtoms::br)) {
        prevListItem = nullptr;
      }
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(content)) {
      // If we met a list element and current list element is not a descendant
      // of the list, append current node to end of the current list element.
      // Then, wrap it with list item element and delete the old container.
      if (curList && !EditorUtils::IsDescendantOf(*content, *curList)) {
        nsresult rv = MoveNodeToEndWithTransaction(*content, *curList);
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
          return EditActionResult(rv);
        }
        CreateElementResult convertListTypeResult =
            ChangeListElementType(MOZ_KnownLive(*content->AsElement()),
                                  aListElementTagName, aListItemElementTagName);
        if (convertListTypeResult.Failed()) {
          NS_WARNING("HTMLEditor::ChangeListElementType() failed");
          return EditActionResult(convertListTypeResult.Rv());
        }
        rv = RemoveBlockContainerWithTransaction(
            MOZ_KnownLive(*convertListTypeResult.GetNewNode()));
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerWithTransaction() failed");
          return EditActionResult(rv);
        }
        prevListItem = nullptr;
        continue;
      }

      // If current list element is in found list element or we've not met a
      // list element, convert current list element to proper type.
      CreateElementResult convertListTypeResult =
          ChangeListElementType(MOZ_KnownLive(*content->AsElement()),
                                aListElementTagName, aListItemElementTagName);
      if (convertListTypeResult.Failed()) {
        NS_WARNING("HTMLEditor::ChangeListElementType() failed");
        return EditActionResult(convertListTypeResult.Rv());
      }
      curList = convertListTypeResult.forget();
      prevListItem = nullptr;
      continue;
    }

    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    MOZ_ASSERT(atContent.IsSetAndValid());
    if (HTMLEditUtils::IsListItem(content)) {
      // If current list item element is not in proper list element, we need
      // to conver the list element.
      if (!atContent.IsContainerHTMLElement(&aListElementTagName)) {
        // If we've not met a list element or current node is not in current
        // list element, insert a list element at current node and set
        // current list element to the new one.
        if (!curList || EditorUtils::IsDescendantOf(*content, *curList)) {
          if (NS_WARN_IF(!atContent.GetContainerAsContent())) {
            return EditActionResult(NS_ERROR_FAILURE);
          }
          ErrorResult error;
          nsCOMPtr<nsIContent> newLeftNode =
              SplitNodeWithTransaction(atContent, error);
          if (NS_WARN_IF(Destroyed())) {
            error.SuppressException();
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (error.Failed()) {
            NS_WARNING("HTMLEditor::SplitNodeWithTransaction() failed");
            return EditActionResult(error.StealNSResult());
          }
          curList = CreateNodeWithTransaction(
              aListElementTagName, EditorDOMPoint(atContent.GetContainer()));
          if (NS_WARN_IF(Destroyed())) {
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (!curList) {
            NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
            return EditActionResult(NS_ERROR_FAILURE);
          }
        }
        // Then, move current node into current list element.
        nsresult rv = MoveNodeToEndWithTransaction(*content, *curList);
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
          return EditActionResult(rv);
        }
        // Convert list item type if current node is different list item type.
        if (!content->IsHTMLElement(&aListItemElementTagName)) {
          RefPtr<Element> newListItemElement = ReplaceContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()), aListItemElementTagName);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (!newListItemElement) {
            NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
            return EditActionResult(NS_ERROR_FAILURE);
          }
        }
      } else {
        // If we've not met a list element, set current list element to the
        // parent of current list item element.
        if (!curList) {
          curList = atContent.GetContainerAsElement();
          NS_WARNING_ASSERTION(
              HTMLEditUtils::IsAnyListElement(curList),
              "Current list item parent is not a list element");
        }
        // If current list item element is not a child of current list element,
        // move it into current list item.
        else if (atContent.GetContainer() != curList) {
          nsresult rv = MoveNodeToEndWithTransaction(*content, *curList);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
            return EditActionResult(rv);
          }
        }
        // Then, if current list item element is not proper type for current
        // list element, convert list item element to proper element.
        if (!content->IsHTMLElement(&aListItemElementTagName)) {
          RefPtr<Element> newListItemElement = ReplaceContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()), aListItemElementTagName);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (!newListItemElement) {
            NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
            return EditActionResult(NS_ERROR_FAILURE);
          }
        }
      }
      Element* element = Element::FromNode(content);
      if (NS_WARN_IF(!element)) {
        return EditActionResult(NS_ERROR_FAILURE);
      }
      // If bullet type is specified, set list type attribute.
      // XXX Cannot we set type attribute before inserting the list item
      //     element into the DOM tree?
      if (!aBulletType.IsEmpty()) {
        nsresult rv = SetAttributeWithTransaction(
            MOZ_KnownLive(*element), *nsGkAtoms::type, aBulletType);
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "EditorBase::SetAttributeWithTransaction(nsGkAtoms::type) "
              "failed");
          return EditActionResult(rv);
        }
        continue;
      }

      // Otherwise, remove list type attribute if there is.
      if (!element->HasAttr(nsGkAtoms::type)) {
        continue;
      }
      nsresult rv = RemoveAttributeWithTransaction(MOZ_KnownLive(*element),
                                                   *nsGkAtoms::type);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::type) "
            "failed");
        return EditActionResult(rv);
      }
      continue;
    }

    MOZ_ASSERT(!HTMLEditUtils::IsAnyListElement(content) &&
               !HTMLEditUtils::IsListItem(content));

    // If current node is a `<div>` element, replace it in the array with
    // its children.
    // XXX I think that this should be done when we collect the nodes above.
    //     Then, we can change this `for` loop to ranged-for loop.
    if (content->IsHTMLElement(nsGkAtoms::div)) {
      prevListItem = nullptr;
      CollectChildren(*content, arrayOfContents, i + 1,
                      CollectListChildren::Yes, CollectTableChildren::Yes,
                      CollectNonEditableNodes::Yes);
      nsresult rv =
          RemoveContainerWithTransaction(MOZ_KnownLive(*content->AsElement()));
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveContainerWithTransaction() failed");
        return EditActionResult(rv);
      }
      // Extend the loop length to handle all children collected here.
      countOfCollectedContents = arrayOfContents.Length();
      continue;
    }

    // If we've not met a list element, create a list element and make it
    // current list element.
    if (!curList) {
      SplitNodeResult splitCurNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(aListElementTagName,
                                                      atContent);
      if (splitCurNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
        return EditActionResult(splitCurNodeResult.Rv());
      }
      prevListItem = nullptr;
      curList = CreateNodeWithTransaction(aListElementTagName,
                                          splitCurNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (!curList) {
        NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
        return EditActionResult(NS_ERROR_FAILURE);
      }
      // Set new block element of top level edit sub-action to the new list
      // element for setting selection into it.
      // XXX This must be wrong.  If we're handling nested edit action,
      //     we shouldn't overwrite the new block element.
      TopLevelEditSubActionDataRef().mNewBlockElement = curList;

      // atContent is now referring the right node with mOffset but
      // referring the left node with mRef.  So, invalidate it now.
      atContent.Clear();
    }

    // If we're currently handling contents of a list item and current node
    // is not a block element, move current node into the list item.
    if (HTMLEditUtils::IsInlineElement(content) && prevListItem) {
      nsresult rv = MoveNodeToEndWithTransaction(*content, *prevListItem);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return EditActionResult(rv);
      }
      continue;
    }

    // If current node is a paragraph, that means that it does not contain
    // block children so that we can just replace it with new list item
    // element and move it into current list element.
    // XXX This is too rough handling.  If web apps modifies DOM tree directly,
    //     any elements can have block elements as children.
    if (content->IsHTMLElement(nsGkAtoms::p)) {
      RefPtr<Element> newListItemElement = ReplaceContainerWithTransaction(
          MOZ_KnownLive(*content->AsElement()), aListItemElementTagName);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (!newListItemElement) {
        NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
        return EditActionResult(NS_ERROR_FAILURE);
      }
      prevListItem = nullptr;
      nsresult rv = MoveNodeToEndWithTransaction(*newListItemElement, *curList);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return EditActionResult(rv);
      }
      // XXX Why don't we set `type` attribute here??
      continue;
    }

    // If current node is not a paragraph, wrap current node with new list
    // item element and move it into current list element.
    RefPtr<Element> newListItemElement =
        InsertContainerWithTransaction(*content, aListItemElementTagName);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (!newListItemElement) {
      NS_WARNING("HTMLEditor::InsertContainerWithTransaction() failed");
      return EditActionResult(NS_ERROR_FAILURE);
    }
    // If current node is not a block element, new list item should have
    // following inline nodes too.
    if (HTMLEditUtils::IsInlineElement(content)) {
      prevListItem = newListItemElement;
    } else {
      prevListItem = nullptr;
    }
    nsresult rv = MoveNodeToEndWithTransaction(*newListItemElement, *curList);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return EditActionResult(rv);
    }
    // XXX Why don't we set `type` attribute here??
  }

  return EditActionHandled();
}

nsresult HTMLEditor::RemoveListAtSelectionAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result.Rv();
  }

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eRemoveList, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return ignoredError.StealNSResult();
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return rv;
    }
  }

  AutoSelectionRestorer restoreSelectionLater(*this);

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoTransactionsConserveSelection dontChangeMySelection(*this);
    nsresult rv =
        SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
            arrayOfContents, EditSubAction::eCreateOrChangeList,
            CollectNonEditableNodes::No);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::"
          "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges("
          "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
      return rv;
    }
  }

  // Remove all non-editable nodes.  Leave them be.
  // XXX SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges()
  //     should return only editable contents when it's called with
  //     CollectNonEditableNodes::No.
  for (int32_t i = arrayOfContents.Length() - 1; i >= 0; i--) {
    OwningNonNull<nsIContent>& content = arrayOfContents[i];
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      arrayOfContents.RemoveElementAt(i);
    }
  }

  // Only act on lists or list items in the array
  for (auto& content : arrayOfContents) {
    // here's where we actually figure out what to do
    if (HTMLEditUtils::IsListItem(content)) {
      // unlist this listitem
      nsresult rv = LiftUpListItemElement(MOZ_KnownLive(*content->AsElement()),
                                          LiftUpFromAllParentListElements::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":Yes) failed");
        return rv;
      }
      continue;
    }
    if (HTMLEditUtils::IsAnyListElement(content)) {
      // node is a list, move list items out
      nsresult rv =
          DestroyListStructureRecursively(MOZ_KnownLive(*content->AsElement()));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DestroyListStructureRecursively() failed");
        return rv;
      }
      continue;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::FormatBlockContainerWithTransaction(nsAtom& blockType) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return rv;
    }
  }

  AutoSelectionRestorer restoreSelectionLater(*this);
  AutoTransactionsConserveSelection dontChangeMySelection(*this);

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
      arrayOfContents, EditSubAction::eCreateOrRemoveBlock,
      CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::"
        "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges("
        "eCreateOrRemoveBlock, CollectNonEditableNodes::Yes) failed");
    return rv;
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (IsEmptyOneHardLine(arrayOfContents)) {
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return NS_ERROR_FAILURE;
    }

    EditorDOMPoint pointToInsertBlock(firstRange->StartRef());
    if (&blockType == nsGkAtoms::normal || &blockType == nsGkAtoms::_empty) {
      if (!pointToInsertBlock.IsInContentNode()) {
        NS_WARNING(
            "HTMLEditor::FormatBlockContainerWithTransaction() couldn't find "
            "block parent because container of the point is not content");
        return NS_ERROR_FAILURE;
      }
      // We are removing blocks (going to "body text")
      RefPtr<Element> blockElement =
          HTMLEditUtils::GetInclusiveAncestorBlockElement(
              *pointToInsertBlock.ContainerAsContent());
      if (!blockElement) {
        NS_WARNING(
            "HTMLEditor::FormatBlockContainerWithTransaction() couldn't find "
            "block parent");
        return NS_ERROR_FAILURE;
      }
      if (!HTMLEditUtils::IsFormatNode(blockElement)) {
        return NS_OK;
      }

      // If the first editable node after selection is a br, consume it.
      // Otherwise it gets pushed into a following block after the split,
      // which is visually bad.
      nsCOMPtr<nsIContent> brContent =
          GetNextEditableHTMLNode(pointToInsertBlock);
      if (brContent && brContent->IsHTMLElement(nsGkAtoms::br)) {
        AutoEditorDOMPointChildInvalidator lockOffset(pointToInsertBlock);
        rv = DeleteNodeWithTransaction(*brContent);
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
          return rv;
        }
      }
      // Do the splits!
      SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
          *blockElement, pointToInsertBlock,
          SplitAtEdges::eDoNotCreateEmptyContainer);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (splitNodeResult.Failed()) {
        NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
        return splitNodeResult.Rv();
      }
      EditorDOMPoint pointToInsertBRNode(splitNodeResult.SplitPoint());
      // Put a <br> element at the split point
      brContent = InsertBRElementWithTransaction(pointToInsertBRNode);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!brContent) {
        NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }
      // Don't restore the selection
      restoreSelectionLater.Abort();
      // Put selection at the split point
      nsresult rv = CollapseSelectionTo(EditorRawDOMPoint(brContent));
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::CollapseSelectionTo() failed");
      return rv;
    }

    // We are making a block.  Consume a br, if needed.
    nsCOMPtr<nsIContent> brNode =
        GetNextEditableHTMLNodeInBlock(pointToInsertBlock);
    if (brNode && brNode->IsHTMLElement(nsGkAtoms::br)) {
      AutoEditorDOMPointChildInvalidator lockOffset(pointToInsertBlock);
      rv = DeleteNodeWithTransaction(*brNode);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
      // We don't need to act on this node any more
      arrayOfContents.RemoveElement(brNode);
    }
    // Make sure we can put a block here.
    SplitNodeResult splitNodeResult =
        MaybeSplitAncestorsForInsertWithTransaction(blockType,
                                                    pointToInsertBlock);
    if (splitNodeResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
      return splitNodeResult.Rv();
    }
    RefPtr<Element> block =
        CreateNodeWithTransaction(blockType, splitNodeResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!block) {
      NS_WARNING("CreateNodeWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }
    // Remember our new block for postprocessing
    TopLevelEditSubActionDataRef().mNewBlockElement = block;
    // Delete anything that was in the list of nodes
    while (!arrayOfContents.IsEmpty()) {
      OwningNonNull<nsIContent>& content = arrayOfContents[0];
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
      arrayOfContents.RemoveElementAt(0);
    }
    // Don't restore the selection
    restoreSelectionLater.Abort();
    // Put selection in new block
    rv = CollapseSelectionToStartOf(*block);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    return rv;
  }
  // Okay, now go through all the nodes and make the right kind of blocks, or
  // whatever is approriate.  Woohoo!  Note: blockquote is handled a little
  // differently.
  if (&blockType == nsGkAtoms::blockquote) {
    nsresult rv = MoveNodesIntoNewBlockquoteElement(arrayOfContents);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::MoveNodesIntoNewBlockquoteElement() failed");
    return rv;
  }
  if (&blockType == nsGkAtoms::normal || &blockType == nsGkAtoms::_empty) {
    nsresult rv = RemoveBlockContainerElements(arrayOfContents);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::RemoveBlockContainerElements() failed");
    return rv;
  }
  rv = CreateOrChangeBlockContainerElement(arrayOfContents, blockType);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::CreateOrChangeBlockContainerElement() failed");
  return rv;
}

nsresult HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (!SelectionRefPtr()->IsCollapsed()) {
    return NS_OK;
  }

  const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }
  const RangeBoundary& atStartOfSelection = firstRange->StartRef();
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  if (!atStartOfSelection.Container()->IsElement()) {
    return NS_OK;
  }
  OwningNonNull<Element> startContainerElement =
      *atStartOfSelection.Container()->AsElement();
  nsresult rv =
      InsertPaddingBRElementForEmptyLastLineIfNeeded(startContainerElement);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertPaddingBRElementForEmptyLastLineIfNeeded() failed");
  return rv;
}

EditActionResult HTMLEditor::IndentAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eIndent, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  result |= HandleIndentAtSelection();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::HandleIndentAtSelection() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  nsresult rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() failed");
  return result.SetResult(rv);
}

// Helper for Handle[CSS|HTML]IndentAtSelectionInternal
nsresult HTMLEditor::IndentListChild(RefPtr<Element>* aCurList,
                                     const EditorDOMPoint& aCurPoint,
                                     nsIContent& aContent) {
  MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(aCurPoint.GetContainer()),
             "unexpected container");
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // some logic for putting list items into nested lists...

  // Check for whether we should join a list that follows aContent.
  // We do this if the next element is a list, and the list is of the
  // same type (li/ol) as aContent was a part it.
  if (nsIContent* nextEditableSibling =
          GetNextHTMLSibling(&aContent, SkipWhiteSpace::Yes)) {
    if (HTMLEditUtils::IsAnyListElement(nextEditableSibling) &&
        aCurPoint.GetContainer()->NodeInfo()->NameAtom() ==
            nextEditableSibling->NodeInfo()->NameAtom() &&
        aCurPoint.GetContainer()->NodeInfo()->NamespaceID() ==
            nextEditableSibling->NodeInfo()->NamespaceID()) {
      nsresult rv = MoveNodeWithTransaction(
          aContent, EditorDOMPoint(nextEditableSibling, 0));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "EdigtorBase::MoveNodeWithTransaction() failed");
      return rv;
    }
  }

  // Check for whether we should join a list that preceeds aContent.
  // We do this if the previous element is a list, and the list is of
  // the same type (li/ol) as aContent was a part of.
  if (nsCOMPtr<nsIContent> previousEditableSibling =
          GetPriorHTMLSibling(&aContent, SkipWhiteSpace::Yes)) {
    if (HTMLEditUtils::IsAnyListElement(previousEditableSibling) &&
        aCurPoint.GetContainer()->NodeInfo()->NameAtom() ==
            previousEditableSibling->NodeInfo()->NameAtom() &&
        aCurPoint.GetContainer()->NodeInfo()->NamespaceID() ==
            previousEditableSibling->NodeInfo()->NamespaceID()) {
      nsresult rv =
          MoveNodeToEndWithTransaction(aContent, *previousEditableSibling);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
  }

  // check to see if aCurList is still appropriate.  Which it is if
  // aContent is still right after it in the same list.
  nsIContent* previousEditableSibling =
      *aCurList ? GetPriorHTMLSibling(&aContent, SkipWhiteSpace::Yes) : nullptr;
  if (!*aCurList ||
      (previousEditableSibling && previousEditableSibling != *aCurList)) {
    nsAtom* containerName = aCurPoint.GetContainer()->NodeInfo()->NameAtom();
    // Create a new nested list of correct type.
    SplitNodeResult splitNodeResult =
        MaybeSplitAncestorsForInsertWithTransaction(
            MOZ_KnownLive(*containerName), aCurPoint);
    if (splitNodeResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
      return splitNodeResult.Rv();
    }
    *aCurList = CreateNodeWithTransaction(MOZ_KnownLive(*containerName),
                                          splitNodeResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!*aCurList) {
      NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }
    // aCurList is now the correct thing to put aContent in
    // remember our new block for postprocessing
    TopLevelEditSubActionDataRef().mNewBlockElement = *aCurList;
  }
  // tuck the node into the end of the active list
  RefPtr<nsINode> container = *aCurList;
  nsresult rv = MoveNodeToEndWithTransaction(aContent, *container);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::MoveNodeToEndWithTransaction() failed");
  return rv;
}

EditActionResult HTMLEditor::HandleIndentAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (IsCSSEnabled()) {
    nsresult rv = HandleCSSIndentAtSelection();
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::HandleCSSIndentAtSelection() failed");
    return EditActionHandled(rv);
  }
  rv = HandleHTMLIndentAtSelection();
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::HandleHTMLIndent() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::HandleCSSIndentAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return rv;
    }
  }

  // HandleCSSIndentAtSelectionInternal() creates AutoSelectionRestorer.
  // Therefore, even if it returns NS_OK, editor might have been destroyed
  // at restoring Selection.
  nsresult rv = HandleCSSIndentAtSelectionInternal();
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::HandleCSSIndentAtSelectionInternal() failed");
  return rv;
}

nsresult HTMLEditor::HandleCSSIndentAtSelectionInternal() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  AutoSelectionRestorer restoreSelectionLater(*this);
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;

  // short circuit: detect case of collapsed selection inside an <li>.
  // just sublist that <li>.  This prevents bug 97797.

  if (SelectionRefPtr()->IsCollapsed()) {
    EditorRawDOMPoint atCaret(EditorBase::GetStartPoint(*SelectionRefPtr()));
    if (NS_WARN_IF(!atCaret.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    MOZ_ASSERT(atCaret.IsInContentNode());
    Element* blockElement = HTMLEditUtils::GetInclusiveAncestorBlockElement(
        *atCaret.ContainerAsContent());
    if (blockElement && HTMLEditUtils::IsListItem(blockElement)) {
      arrayOfContents.AppendElement(*blockElement);
    }
  }

  if (arrayOfContents.IsEmpty()) {
    nsresult rv =
        SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
            arrayOfContents, EditSubAction::eIndent,
            CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::"
          "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges("
          "eIndent, CollectNonEditableNodes::Yes) failed");
      return rv;
    }
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (IsEmptyOneHardLine(arrayOfContents)) {
    // get selection location
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return NS_ERROR_FAILURE;
    }

    EditorDOMPoint atStartOfSelection(firstRange->StartRef());
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // make sure we can put a block here
    SplitNodeResult splitNodeResult =
        MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::div,
                                                    atStartOfSelection);
    if (splitNodeResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms::"
          "div) failed");
      return splitNodeResult.Rv();
    }
    RefPtr<Element> theBlock = CreateNodeWithTransaction(
        *nsGkAtoms::div, splitNodeResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!theBlock) {
      NS_WARNING(
          "EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
      return NS_ERROR_FAILURE;
    }
    // remember our new block for postprocessing
    TopLevelEditSubActionDataRef().mNewBlockElement = theBlock;
    nsresult rv = ChangeMarginStart(*theBlock, ChangeMargin::Increase);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::ChangeMarginStart() failed, but ignored");
    // delete anything that was in the list of nodes
    // XXX We don't need to remove the nodes from the array for performance.
    while (!arrayOfContents.IsEmpty()) {
      OwningNonNull<nsIContent>& content = arrayOfContents[0];
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
      arrayOfContents.RemoveElementAt(0);
    }
    // Don't restore the selection
    restoreSelectionLater.Abort();
    // put selection in new block
    rv = CollapseSelectionToStartOf(*theBlock);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    return rv;
  }

  // Ok, now go through all the nodes and put them in a blockquote,
  // or whatever is appropriate.
  RefPtr<Element> curList, curQuote;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Ignore all non-editable nodes.  Leave them be.
    // XXX We ignore non-editable nodes here, but not so in the above block.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      nsresult rv =
          IndentListChild(&curList, atContent, MOZ_KnownLive(content));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::IndentListChild() failed");
        return rv;
      }
      continue;
    }

    // Not a list item.

    if (HTMLEditUtils::IsBlockElement(content)) {
      nsresult rv = ChangeMarginStart(MOZ_KnownLive(*content->AsElement()),
                                      ChangeMargin::Increase);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::ChangeMarginStart() failed, but ignored");
      curQuote = nullptr;
      continue;
    }

    if (!curQuote) {
      // First, check that our element can contain a div.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::div)) {
        return NS_OK;  // cancelled
      }

      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::div,
                                                      atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms:"
            ":div) failed");
        return splitNodeResult.Rv();
      }
      curQuote = CreateNodeWithTransaction(*nsGkAtoms::div,
                                           splitNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!curQuote) {
        NS_WARNING(
            "EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
        return NS_ERROR_FAILURE;
      }
      nsresult rv = ChangeMarginStart(*curQuote, ChangeMargin::Increase);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::ChangeMarginStart() failed, but ignored");
      // remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = curQuote;
      // curQuote is now the correct thing to put content in
    }

    // tuck the node into the end of the active blockquote
    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
    // keep it alive.
    nsresult rv =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curQuote);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::HandleHTMLIndentAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return rv;
    }
  }

  // HandleHTMLIndentAtSelectionInternal() creates AutoSelectionRestorer.
  // Therefore, even if it returns NS_OK, editor might have been destroyed
  // at restoring Selection.
  nsresult rv = HandleHTMLIndentAtSelectionInternal();
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::HandleHTMLIndentAtSelectionInternal() failed");
  return rv;
}

nsresult HTMLEditor::HandleHTMLIndentAtSelectionInternal() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  AutoSelectionRestorer restoreSelectionLater(*this);

  // convert the selection ranges into "promoted" selection ranges:
  // this basically just expands the range to include the immediate
  // block parent, and then further expands to include any ancestors
  // whose children are all in the range

  AutoTArray<RefPtr<nsRange>, 4> arrayOfRanges;
  GetSelectionRangesExtendedToHardLineStartAndEnd(arrayOfRanges,
                                                  EditSubAction::eIndent);

  // use these ranges to construct a list of nodes to act on.
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodes(
      arrayOfRanges, arrayOfContents, EditSubAction::eIndent,
      CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::SplitInlinesAndCollectEditTargetNodes(eIndent, "
        "CollectNonEditableNodes::Yes) failed");
    return rv;
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (IsEmptyOneHardLine(arrayOfContents)) {
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return NS_ERROR_FAILURE;
    }

    EditorDOMPoint atStartOfSelection(firstRange->StartRef());
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // Make sure we can put a block here.
    SplitNodeResult splitNodeResult =
        MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::blockquote,
                                                    atStartOfSelection);
    if (splitNodeResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms::"
          "blockquote) failed");
      return splitNodeResult.Rv();
    }
    RefPtr<Element> theBlock = CreateNodeWithTransaction(
        *nsGkAtoms::blockquote, splitNodeResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!theBlock) {
      NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }
    // remember our new block for postprocessing
    TopLevelEditSubActionDataRef().mNewBlockElement = theBlock;
    // delete anything that was in the list of nodes
    // XXX We don't need to remove the nodes from the array for performance.
    while (!arrayOfContents.IsEmpty()) {
      OwningNonNull<nsIContent>& content = arrayOfContents[0];
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
      arrayOfContents.RemoveElementAt(0);
    }
    // Don't restore the selection
    restoreSelectionLater.Abort();
    nsresult rv = CollapseSelectionToStartOf(*theBlock);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    return rv;
  }

  // Ok, now go through all the nodes and put them in a blockquote,
  // or whatever is appropriate.  Wohoo!
  RefPtr<Element> curList, curQuote, indentedLI;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Ignore all non-editable nodes.  Leave them be.
    // XXX We ignore non-editable nodes here, but not so in the above block.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      nsresult rv =
          IndentListChild(&curList, atContent, MOZ_KnownLive(content));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::IndentListChild() failed");
        return rv;
      }
      // forget curQuote, if any
      curQuote = nullptr;
      continue;
    }

    // Not a list item, use blockquote?

    // if we are inside a list item, we don't want to blockquote, we want
    // to sublist the list item.  We may have several nodes listed in the
    // array of nodes to act on, that are in the same list item.  Since
    // we only want to indent that li once, we must keep track of the most
    // recent indented list item, and not indent it if we find another node
    // to act on that is still inside the same li.
    if (RefPtr<Element> listItem = GetNearestAncestorListItemElement(content)) {
      if (indentedLI == listItem) {
        // already indented this list item
        continue;
      }
      // check to see if curList is still appropriate.  Which it is if
      // content is still right after it in the same list.
      nsIContent* previousEditableSibling =
          curList ? GetPriorHTMLSibling(listItem) : nullptr;
      if (!curList ||
          (previousEditableSibling && previousEditableSibling != curList)) {
        EditorDOMPoint atListItem(listItem);
        if (NS_WARN_IF(!listItem)) {
          return NS_ERROR_FAILURE;
        }
        nsAtom* containerName =
            atListItem.GetContainer()->NodeInfo()->NameAtom();
        // Create a new nested list of correct type.
        SplitNodeResult splitNodeResult =
            MaybeSplitAncestorsForInsertWithTransaction(
                MOZ_KnownLive(*containerName), atListItem);
        if (splitNodeResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() "
              "failed");
          return splitNodeResult.Rv();
        }
        curList = CreateNodeWithTransaction(MOZ_KnownLive(*containerName),
                                            splitNodeResult.SplitPoint());
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (!curList) {
          NS_WARNING("HTMLEditor::CreateNodeWithTransaction() failed");
          return NS_ERROR_FAILURE;
        }
      }

      rv = MoveNodeToEndWithTransaction(*listItem, *curList);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return rv;
      }

      // remember we indented this li
      indentedLI = listItem;

      continue;
    }

    // need to make a blockquote to put things in if we haven't already,
    // or if this node doesn't go in blockquote we used earlier.
    // One reason it might not go in prio blockquote is if we are now
    // in a different table cell.
    if (curQuote &&
        HTMLEditor::NodesInDifferentTableElements(*curQuote, content)) {
      curQuote = nullptr;
    }

    if (!curQuote) {
      // First, check that our element can contain a blockquote.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::blockquote)) {
        return NS_OK;  // cancelled
      }

      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::blockquote,
                                                      atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms:"
            ":blockquote) failed");
        return splitNodeResult.Rv();
      }
      curQuote = CreateNodeWithTransaction(*nsGkAtoms::blockquote,
                                           splitNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!curQuote) {
        NS_WARNING(
            "EditorBase::CreateNodeWithTransaction(nsGkAtoms::blockquote) "
            "failed");
        return NS_ERROR_FAILURE;
      }
      // remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = curQuote;
      // curQuote is now the correct thing to put curNode in
    }

    // tuck the node into the end of the active blockquote
    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
    // keep it alive.
    rv = MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curQuote);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
    // forget curList, if any
    curList = nullptr;
  }
  return NS_OK;
}

EditActionResult HTMLEditor::OutdentAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eOutdent, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  result |= HandleOutdentAtSelection();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::HandleOutdentAtSelection() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  nsresult rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() "
      "failed");
  return result.SetResult(rv);
}

EditActionResult HTMLEditor::HandleOutdentAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return EditActionHandled(rv);
    }
  }

  // HandleOutdentAtSelectionInternal() creates AutoSelectionRestorer.
  // Therefore, even if it returns NS_OK, the editor might have been destroyed
  // at restoring Selection.
  SplitRangeOffFromNodeResult outdentResult =
      HandleOutdentAtSelectionInternal();
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (outdentResult.Failed()) {
    NS_WARNING("HTMLEditor::HandleOutdentAtSelectionInternal() failed");
    return EditActionHandled(outdentResult.Rv());
  }

  // Make sure selection didn't stick to last piece of content in old bq (only
  // a problem for collapsed selections)
  if (!outdentResult.GetLeftContent() && !outdentResult.GetRightContent()) {
    return EditActionHandled();
  }

  if (!SelectionRefPtr()->IsCollapsed()) {
    return EditActionHandled();
  }

  // Push selection past end of left element of last split indented element.
  if (outdentResult.GetLeftContent()) {
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionHandled();
    }
    const RangeBoundary& atStartOfSelection = firstRange->StartRef();
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionHandled(NS_ERROR_FAILURE);
    }
    if (atStartOfSelection.Container() == outdentResult.GetLeftContent() ||
        EditorUtils::IsDescendantOf(*atStartOfSelection.Container(),
                                    *outdentResult.GetLeftContent())) {
      // Selection is inside the left node - push it past it.
      EditorRawDOMPoint afterRememberedLeftBQ(
          EditorRawDOMPoint::After(*outdentResult.GetLeftContent()));
      NS_WARNING_ASSERTION(
          afterRememberedLeftBQ.IsSet(),
          "Failed to set after remembered left blockquote element");
      nsresult rv = CollapseSelectionTo(afterRememberedLeftBQ);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::CollapseSelectionTo() failed, but ignored");
    }
  }
  // And pull selection before beginning of right element of last split
  // indented element.
  if (outdentResult.GetRightContent()) {
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionHandled();
    }
    const RangeBoundary& atStartOfSelection = firstRange->StartRef();
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionHandled(NS_ERROR_FAILURE);
    }
    if (atStartOfSelection.Container() == outdentResult.GetRightContent() ||
        EditorUtils::IsDescendantOf(*atStartOfSelection.Container(),
                                    *outdentResult.GetRightContent())) {
      // Selection is inside the right element - push it before it.
      EditorRawDOMPoint atRememberedRightBQ(outdentResult.GetRightContent());
      nsresult rv = CollapseSelectionTo(atRememberedRightBQ);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::CollapseSelectionTo() failed, but ignored");
    }
  }
  return EditActionHandled();
}

SplitRangeOffFromNodeResult HTMLEditor::HandleOutdentAtSelectionInternal() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoSelectionRestorer restoreSelectionLater(*this);

  bool useCSS = IsCSSEnabled();

  // Convert the selection ranges into "promoted" selection ranges: this
  // basically just expands the range to include the immediate block parent,
  // and then further expands to include any ancestors whose children are all
  // in the range
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
      arrayOfContents, EditSubAction::eOutdent, CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::"
        "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges() "
        "failed");
    return SplitRangeOffFromNodeResult(rv);
  }

  nsCOMPtr<nsIContent> leftContentOfLastOutdented;
  nsCOMPtr<nsIContent> middleContentOfLastOutdented;
  nsCOMPtr<nsIContent> rightContentOfLastOutdented;
  RefPtr<Element> indentedParentElement;
  nsCOMPtr<nsIContent> firstContentToBeOutdented, lastContentToBeOutdented;
  BlockIndentedWith indentedParentIndentedWith = BlockIndentedWith::HTML;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do
    EditorDOMPoint atContent(content);
    if (!atContent.IsSet()) {
      continue;
    }

    // If it's a `<blockquote>`, remove it to outdent its children.
    if (content->IsHTMLElement(nsGkAtoms::blockquote)) {
      // If we've already found an ancestor block element indented, we need to
      // split it and remove the block element first.
      if (indentedParentElement) {
        MOZ_ASSERT(indentedParentElement == content);
        SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
            *indentedParentElement, *firstContentToBeOutdented,
            *lastContentToBeOutdented, indentedParentIndentedWith);
        if (outdentResult.Failed()) {
          NS_WARNING("HTMLEditor::OutdentPartOfBlock() failed");
          return outdentResult;
        }
        leftContentOfLastOutdented = outdentResult.GetLeftContent();
        middleContentOfLastOutdented = outdentResult.GetMiddleContent();
        rightContentOfLastOutdented = outdentResult.GetRightContent();
        indentedParentElement = nullptr;
        firstContentToBeOutdented = nullptr;
        lastContentToBeOutdented = nullptr;
        indentedParentIndentedWith = BlockIndentedWith::HTML;
      }
      nsresult rv = RemoveBlockContainerWithTransaction(
          MOZ_KnownLive(*content->AsElement()));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    // If we're using CSS and the node is a block element, check its start
    // margin whether it's indented with CSS.
    if (useCSS && HTMLEditUtils::IsBlockElement(content)) {
      nsStaticAtom& marginProperty =
          MarginPropertyAtomForIndent(MOZ_KnownLive(content));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      nsAutoString value;
      DebugOnly<nsresult> rvIgnored =
          CSSEditUtils::GetSpecifiedProperty(content, marginProperty, value);
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
      float startMargin = 0;
      RefPtr<nsAtom> unit;
      CSSEditUtils::ParseLength(value, &startMargin, getter_AddRefs(unit));
      // If indented with CSS, we should decrease the start mergin.
      if (startMargin > 0) {
        nsresult rv = ChangeMarginStart(MOZ_KnownLive(*content->AsElement()),
                                        ChangeMargin::Decrease);
        if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
          return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
        }
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "HTMLEditor::ChangeMarginStart(ChangeMargin::"
                             "Decrease) failed, but ignored");
        continue;
      }
    }

    // If it's a list item, we should treat as that it "indents" its children.
    if (HTMLEditUtils::IsListItem(content)) {
      // If it is a list item, that means we are not outdenting whole list.
      // XXX I don't understand this sentence...  We may meet parent list
      //     element, no?
      if (indentedParentElement) {
        SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
            *indentedParentElement, *firstContentToBeOutdented,
            *lastContentToBeOutdented, indentedParentIndentedWith);
        if (NS_WARN_IF(outdentResult.Failed())) {
          return outdentResult;
        }
        leftContentOfLastOutdented = outdentResult.GetLeftContent();
        middleContentOfLastOutdented = outdentResult.GetMiddleContent();
        rightContentOfLastOutdented = outdentResult.GetRightContent();
        indentedParentElement = nullptr;
        firstContentToBeOutdented = nullptr;
        lastContentToBeOutdented = nullptr;
        indentedParentIndentedWith = BlockIndentedWith::HTML;
      }
      // XXX `content` could become different element since
      //     `OutdentPartOfBlock()` may run mutation event listeners.
      rv = LiftUpListItemElement(MOZ_KnownLive(*content->AsElement()),
                                 LiftUpFromAllParentListElements::No);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":No) failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    // If we've found an ancestor block element which indents its children
    // and the current node is NOT a descendant of it, we should remove it to
    // outdent its children.  Otherwise, i.e., current node is a descendant of
    // it, we meet new node which should be outdented when the indented parent
    // is removed.
    if (indentedParentElement) {
      if (EditorUtils::IsDescendantOf(*content, *indentedParentElement)) {
        // Extend the range to be outdented at removing the
        // indentedParentElement.
        lastContentToBeOutdented = content;
        continue;
      }
      SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
          *indentedParentElement, *firstContentToBeOutdented,
          *lastContentToBeOutdented, indentedParentIndentedWith);
      if (outdentResult.Failed()) {
        NS_WARNING("HTMLEditor::OutdentPartOfBlock() failed");
        return outdentResult;
      }
      leftContentOfLastOutdented = outdentResult.GetLeftContent();
      middleContentOfLastOutdented = outdentResult.GetMiddleContent();
      rightContentOfLastOutdented = outdentResult.GetRightContent();
      indentedParentElement = nullptr;
      firstContentToBeOutdented = nullptr;
      lastContentToBeOutdented = nullptr;
      // curBlockIndentedWith = HTMLEditor::BlockIndentedWith::HTML;

      // Then, we need to look for next indentedParentElement.
    }

    indentedParentIndentedWith = BlockIndentedWith::HTML;
    RefPtr<Element> editingHost = GetActiveEditingHost();
    for (nsCOMPtr<nsIContent> parentContent = content->GetParent();
         parentContent && !parentContent->IsHTMLElement(nsGkAtoms::body) &&
         parentContent != editingHost &&
         (parentContent->IsHTMLElement(nsGkAtoms::table) ||
          !HTMLEditUtils::IsAnyTableElement(parentContent));
         parentContent = parentContent->GetParent()) {
      // If we reach a `<blockquote>` ancestor, it should be split at next
      // time at least for outdenting current node.
      if (parentContent->IsHTMLElement(nsGkAtoms::blockquote)) {
        indentedParentElement = parentContent->AsElement();
        firstContentToBeOutdented = content;
        lastContentToBeOutdented = content;
        break;
      }

      if (!useCSS) {
        continue;
      }

      nsCOMPtr<nsINode> grandParentNode = parentContent->GetParentNode();
      nsStaticAtom& marginProperty =
          MarginPropertyAtomForIndent(MOZ_KnownLive(content));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_WARN_IF(grandParentNode != parentContent->GetParentNode())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      nsAutoString value;
      DebugOnly<nsresult> rvIgnored = CSSEditUtils::GetSpecifiedProperty(
          *parentContent, marginProperty, value);
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
      // XXX Now, editing host may become different element.  If so, shouldn't
      //     we stop this handling?
      float startMargin;
      RefPtr<nsAtom> unit;
      CSSEditUtils::ParseLength(value, &startMargin, getter_AddRefs(unit));
      // If we reach a block element which indents its children with start
      // margin, we should remove it at next time.
      if (startMargin > 0 &&
          !(HTMLEditUtils::IsAnyListElement(atContent.GetContainer()) &&
            HTMLEditUtils::IsAnyListElement(content))) {
        indentedParentElement = parentContent->AsElement();
        firstContentToBeOutdented = content;
        lastContentToBeOutdented = content;
        indentedParentIndentedWith = BlockIndentedWith::CSS;
        break;
      }
    }

    if (indentedParentElement) {
      continue;
    }

    // If we don't have any block elements which indents current node and
    // both current node and its parent are list element, remove current
    // node to move all its children to the parent list.
    // XXX This is buggy.  When both lists' item types are different,
    //     we create invalid tree.  E.g., `<ul>` may have `<dd>` as its
    //     list item element.
    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      // Move node out of list
      if (HTMLEditUtils::IsAnyListElement(content)) {
        // Just unwrap this sublist
        nsresult rv = RemoveBlockContainerWithTransaction(
            MOZ_KnownLive(*content->AsElement()));
        if (NS_WARN_IF(Destroyed())) {
          return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerWithTransaction() failed");
          return SplitRangeOffFromNodeResult(rv);
        }
      }
      continue;
    }

    // If current content is a list element but its parent is not a list
    // element, move children to where it is and remove it from the tree.
    if (HTMLEditUtils::IsAnyListElement(content)) {
      // XXX If mutation event listener appends new children forever, this
      //     becomes an infinite loop so that we should set limitation from
      //     first child count.
      for (nsCOMPtr<nsIContent> lastChildContent = content->GetLastChild();
           lastChildContent; lastChildContent = content->GetLastChild()) {
        if (HTMLEditUtils::IsListItem(lastChildContent)) {
          nsresult rv = LiftUpListItemElement(
              MOZ_KnownLive(*lastChildContent->AsElement()),
              LiftUpFromAllParentListElements::No);
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "HTMLEditor::LiftUpListItemElement("
                "LiftUpFromAllParentListElements::No) failed");
            return SplitRangeOffFromNodeResult(rv);
          }
          continue;
        }

        if (HTMLEditUtils::IsAnyListElement(lastChildContent)) {
          // We have an embedded list, so move it out from under the parent
          // list. Be sure to put it after the parent list because this
          // loop iterates backwards through the parent's list of children.
          EditorDOMPoint afterCurrentList(EditorDOMPoint::After(atContent));
          NS_WARNING_ASSERTION(
              afterCurrentList.IsSet(),
              "Failed to set it to after current list element");
          nsresult rv =
              MoveNodeWithTransaction(*lastChildContent, afterCurrentList);
          if (NS_WARN_IF(Destroyed())) {
            return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
            return SplitRangeOffFromNodeResult(rv);
          }
          continue;
        }

        // Delete any non-list items for now
        // XXX Chrome moves it from the list element.  We should follow it.
        nsresult rv = DeleteNodeWithTransaction(*lastChildContent);
        if (NS_WARN_IF(Destroyed())) {
          return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
          return SplitRangeOffFromNodeResult(rv);
        }
      }
      // Delete the now-empty list
      nsresult rv = RemoveBlockContainerWithTransaction(
          MOZ_KnownLive(*content->AsElement()));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    if (useCSS) {
      if (RefPtr<Element> element = content->GetAsElementOrParentElement()) {
        nsresult rv = ChangeMarginStart(*element, ChangeMargin::Decrease);
        if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
          return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
        }
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "HTMLEditor::ChangeMarginStart(ChangeMargin::"
                             "Decrease) failed, but ignored");
      }
      continue;
    }
  }

  if (!indentedParentElement) {
    return SplitRangeOffFromNodeResult(leftContentOfLastOutdented,
                                       middleContentOfLastOutdented,
                                       rightContentOfLastOutdented);
  }

  // We have a <blockquote> we haven't finished handling.
  SplitRangeOffFromNodeResult outdentResult =
      OutdentPartOfBlock(*indentedParentElement, *firstContentToBeOutdented,
                         *lastContentToBeOutdented, indentedParentIndentedWith);
  NS_WARNING_ASSERTION(outdentResult.Succeeded(),
                       "HTMLEditor::OutdentPartOfBlock() failed");
  return outdentResult;
}

SplitRangeOffFromNodeResult
HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer(
    Element& aBlockElement, nsIContent& aStartOfRange,
    nsIContent& aEndOfRange) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  SplitRangeOffFromNodeResult splitResult =
      SplitRangeOffFromBlock(aBlockElement, aStartOfRange, aEndOfRange);
  if (NS_WARN_IF(splitResult.Rv() == NS_ERROR_EDITOR_DESTROYED)) {
    return splitResult;
  }
  NS_WARNING_ASSERTION(
      splitResult.Succeeded(),
      "HTMLEditor::SplitRangeOffFromBlock() failed, but might be ignored");
  nsresult rv = RemoveBlockContainerWithTransaction(aBlockElement);
  if (NS_WARN_IF(Destroyed())) {
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
    return SplitRangeOffFromNodeResult(rv);
  }
  return SplitRangeOffFromNodeResult(splitResult.GetLeftContent(), nullptr,
                                     splitResult.GetRightContent());
}

SplitRangeOffFromNodeResult HTMLEditor::SplitRangeOffFromBlock(
    Element& aBlockElement, nsIContent& aStartOfMiddleElement,
    nsIContent& aEndOfMiddleElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // aStartOfMiddleElement and aEndOfMiddleElement must be exclusive
  // descendants of aBlockElement.
  MOZ_ASSERT(EditorUtils::IsDescendantOf(aStartOfMiddleElement, aBlockElement));
  MOZ_ASSERT(EditorUtils::IsDescendantOf(aEndOfMiddleElement, aBlockElement));

  // Split at the start.
  SplitNodeResult splitAtStartResult = SplitNodeDeepWithTransaction(
      aBlockElement, EditorDOMPoint(&aStartOfMiddleElement),
      SplitAtEdges::eDoNotCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(splitAtStartResult.Succeeded(),
                       "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
                       "eDoNotCreateEmptyContainer) failed");

  // Split at after the end
  EditorDOMPoint atAfterEnd(&aEndOfMiddleElement);
  DebugOnly<bool> advanced = atAfterEnd.AdvanceOffset();
  NS_WARNING_ASSERTION(advanced, "Failed to advance offset after the end node");
  SplitNodeResult splitAtEndResult = SplitNodeDeepWithTransaction(
      aBlockElement, atAfterEnd, SplitAtEdges::eDoNotCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(splitAtEndResult.Succeeded(),
                       "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
                       "eDoNotCreateEmptyContainer) failed");

  return SplitRangeOffFromNodeResult(splitAtStartResult, splitAtEndResult);
}

SplitRangeOffFromNodeResult HTMLEditor::OutdentPartOfBlock(
    Element& aBlockElement, nsIContent& aStartOfOutdent,
    nsIContent& aEndOfOutdent, BlockIndentedWith aBlockIndentedWith) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  SplitRangeOffFromNodeResult splitResult =
      SplitRangeOffFromBlock(aBlockElement, aStartOfOutdent, aEndOfOutdent);
  if (NS_WARN_IF(splitResult.EditorDestroyed())) {
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }

  if (!splitResult.GetMiddleContentAsElement()) {
    NS_WARNING(
        "HTMLEditor::SplitRangeOffFromBlock() didn't return middle content");
    return SplitRangeOffFromNodeResult(NS_ERROR_FAILURE);
  }
  NS_WARNING_ASSERTION(
      splitResult.Succeeded(),
      "HTMLEditor::SplitRangeOffFromBlock() failed, but might be ignored");

  if (aBlockIndentedWith == BlockIndentedWith::HTML) {
    nsresult rv = RemoveBlockContainerWithTransaction(
        MOZ_KnownLive(*splitResult.GetMiddleContentAsElement()));
    if (NS_WARN_IF(Destroyed())) {
      return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
      return SplitRangeOffFromNodeResult(rv);
    }
    return SplitRangeOffFromNodeResult(splitResult.GetLeftContent(), nullptr,
                                       splitResult.GetRightContent());
  }

  if (splitResult.GetMiddleContentAsElement()) {
    nsresult rv = ChangeMarginStart(
        MOZ_KnownLive(*splitResult.GetMiddleContentAsElement()),
        ChangeMargin::Decrease);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::ChangeMarginStart(ChangeMargin::Decrease) failed");
      return SplitRangeOffFromNodeResult(rv);
    }
    return splitResult;
  }

  return splitResult;
}

CreateElementResult HTMLEditor::ChangeListElementType(Element& aListElement,
                                                      nsAtom& aNewListTag,
                                                      nsAtom& aNewListItemTag) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  for (nsIContent* childContent = aListElement.GetFirstChild(); childContent;
       childContent = childContent->GetNextSibling()) {
    if (!childContent->IsElement()) {
      continue;
    }
    if (HTMLEditUtils::IsListItem(childContent->AsElement()) &&
        !childContent->IsHTMLElement(&aNewListItemTag)) {
      OwningNonNull<Element> listItemElement = *childContent->AsElement();
      RefPtr<Element> newListItemElement =
          ReplaceContainerWithTransaction(listItemElement, aNewListItemTag);
      if (NS_WARN_IF(Destroyed())) {
        return CreateElementResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (!newListItemElement) {
        NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
        return CreateElementResult(NS_ERROR_FAILURE);
      }
      childContent = newListItemElement;
      continue;
    }
    if (HTMLEditUtils::IsAnyListElement(childContent->AsElement()) &&
        !childContent->IsHTMLElement(&aNewListTag)) {
      // XXX List elements shouldn't have other list elements as their
      //     child.  Why do we handle such invalid tree?
      //     -> Maybe, for bug 525888.
      OwningNonNull<Element> listElement = *childContent->AsElement();
      CreateElementResult convertListTypeResult =
          ChangeListElementType(listElement, aNewListTag, aNewListItemTag);
      if (convertListTypeResult.Failed()) {
        NS_WARNING("HTMLEditor::ChangeListElementType() failed");
        return convertListTypeResult;
      }
      childContent = convertListTypeResult.GetNewNode();
      continue;
    }
  }

  if (aListElement.IsHTMLElement(&aNewListTag)) {
    return CreateElementResult(&aListElement);
  }

  RefPtr<Element> listElement =
      ReplaceContainerWithTransaction(aListElement, aNewListTag);
  if (NS_WARN_IF(Destroyed())) {
    return CreateElementResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(listElement,
                       "HTMLEditor::ReplaceContainerWithTransaction() failed");
  return CreateElementResult(std::move(listElement));
}

nsresult HTMLEditor::CreateStyleForInsertText(
    const AbstractRange& aAbstractRange) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aAbstractRange.IsPositioned());
  MOZ_ASSERT(mTypeInState);

  RefPtr<Element> documentRootElement = GetDocument()->GetRootElement();
  if (NS_WARN_IF(!documentRootElement)) {
    return NS_ERROR_FAILURE;
  }

  // process clearing any styles first
  UniquePtr<PropItem> item = mTypeInState->TakeClearProperty();

  EditorDOMPoint pointToPutCaret(aAbstractRange.StartRef());
  bool putCaret = false;
  {
    // Transactions may set selection, but we will set selection if necessary.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    while (item && pointToPutCaret.GetContainer() != documentRootElement) {
      EditResult result = ClearStyleAt(
          pointToPutCaret, MOZ_KnownLive(item->tag), MOZ_KnownLive(item->attr));
      if (result.Failed()) {
        NS_WARNING("HTMLEditor::ClearStyleAt() failed");
        return result.Rv();
      }
      pointToPutCaret = result.PointRefToCollapseSelection();
      item = mTypeInState->TakeClearProperty();
      putCaret = true;
    }
  }

  // then process setting any styles
  int32_t relFontSize = mTypeInState->TakeRelativeFontSize();
  item = mTypeInState->TakeSetProperty();

  if (item || relFontSize) {
    // we have at least one style to add; make a new text node to insert style
    // nodes above.
    if (pointToPutCaret.IsInTextNode()) {
      // if we are in a text node, split it
      SplitNodeResult splitTextNodeResult = SplitNodeDeepWithTransaction(
          MOZ_KnownLive(*pointToPutCaret.GetContainerAsText()), pointToPutCaret,
          SplitAtEdges::eAllowToCreateEmptyContainer);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (splitTextNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eAllowToCreateEmptyContainer) failed");
        return splitTextNodeResult.Rv();
      }
      pointToPutCaret = splitTextNodeResult.SplitPoint();
    }
    if (!pointToPutCaret.IsInContentNode() ||
        !HTMLEditUtils::IsContainerNode(
            *pointToPutCaret.ContainerAsContent())) {
      return NS_OK;
    }
    RefPtr<Text> newEmptyTextNode = CreateTextNode(u""_ns);
    if (!newEmptyTextNode) {
      NS_WARNING("EditorBase::CreateTextNode() failed");
      return NS_ERROR_FAILURE;
    }
    nsresult rv = InsertNodeWithTransaction(*newEmptyTextNode, pointToPutCaret);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::InsertNodeWithTransaction() failed");
      return rv;
    }
    pointToPutCaret.Set(newEmptyTextNode, 0);
    putCaret = true;

    if (relFontSize) {
      // dir indicated bigger versus smaller.  1 = bigger, -1 = smaller
      HTMLEditor::FontSize dir = relFontSize > 0 ? HTMLEditor::FontSize::incr
                                                 : HTMLEditor::FontSize::decr;
      for (int32_t j = 0; j < DeprecatedAbs(relFontSize); j++) {
        nsresult rv =
            RelativeFontChangeOnTextNode(dir, *newEmptyTextNode, 0, -1);
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::RelativeFontChangeOnTextNode() failed");
          return rv;
        }
      }
    }

    while (item) {
      nsresult rv = SetInlinePropertyOnNode(
          MOZ_KnownLive(*pointToPutCaret.GetContainerAsContent()),
          MOZ_KnownLive(*item->tag), MOZ_KnownLive(item->attr), item->value);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::SetInlinePropertyOnNode() failed");
        return rv;
      }
      item = mTypeInState->TakeSetProperty();
    }
  }

  if (!putCaret) {
    return NS_OK;
  }

  nsresult rv = CollapseSelectionTo(pointToPutCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

bool HTMLEditor::IsEmptyBlockElement(Element& aElement,
                                     IgnoreSingleBR aIgnoreSingleBR) const {
  if (!HTMLEditUtils::IsBlockElement(aElement)) {
    return false;
  }
  return IsEmptyNode(aElement, aIgnoreSingleBR == IgnoreSingleBR::Yes);
}

EditActionResult HTMLEditor::AlignAsSubAction(const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetOrClearAlignment, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(rv);
    }
  }

  // AlignContentsAtSelection() creates AutoSelectionRestorer.  Therefore,
  // we need to check whether we've been destroyed or not even if it returns
  // NS_OK.
  rv = AlignContentsAtSelection(aAlignType);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::AlignContentsAtSelection() failed");
    return EditActionHandled(rv);
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() "
      "failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::AlignContentsAtSelection(const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  AutoSelectionRestorer restoreSelectionLater(*this);

  // Convert the selection ranges into "promoted" selection ranges: This
  // basically just expands the range to include the immediate block parent,
  // and then further expands to include any ancestors whose children are all
  // in the range
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges(
      arrayOfContents, EditSubAction::eSetOrClearAlignment,
      CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::"
        "SplitInlinesAndCollectEditTargetNodesInExtendedSelectionRanges("
        "eSetOrClearAlignment, CollectNonEditableNodes::Yes) failed");
    return rv;
  }

  // If we don't have any nodes, or we have only a single br, then we are
  // creating an empty alignment div.  We have to do some different things for
  // these.
  bool createEmptyDivElement = arrayOfContents.IsEmpty();
  if (arrayOfContents.Length() == 1) {
    OwningNonNull<nsIContent>& content = arrayOfContents[0];

    if (HTMLEditUtils::SupportsAlignAttr(content)) {
      // The node is a table element, an hr, a paragraph, a div or a section
      // header; in HTML 4, it can directly carry the ALIGN attribute and we
      // don't need to make a div! If we are in CSS mode, all the work is done
      // in SetBlockElementAlign().
      nsresult rv =
          SetBlockElementAlign(MOZ_KnownLive(*content->AsElement()), aAlignType,
                               EditTarget::OnlyDescendantsExceptTable);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::SetBlockElementAlign() failed");
      return rv;
    }

    if (content->IsHTMLElement(nsGkAtoms::br)) {
      // The special case createEmptyDivElement code (below) that consumes
      // `<br>` elements can cause tables to split if the start node of the
      // selection is not in a table cell or caption, for example parent is a
      // `<tr>`.  Avoid this unnecessary splitting if possible by leaving
      // createEmptyDivElement false so that we fall through to the normal case
      // alignment code.
      //
      // XXX: It seems a little error prone for the createEmptyDivElement
      //      special case code to assume that the start node of the selection
      //      is the parent of the single node in the arrayOfContents, as the
      //      paragraph above points out. Do we rely on the selection start
      //      node because of the fact that arrayOfContents can be empty?  We
      //      should probably revisit this issue. - kin

      const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
      if (NS_WARN_IF(!firstRange)) {
        return NS_ERROR_FAILURE;
      }
      const RangeBoundary& atStartOfSelection = firstRange->StartRef();
      if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
        return NS_ERROR_FAILURE;
      }
      nsINode* parent = atStartOfSelection.Container();
      createEmptyDivElement = !HTMLEditUtils::IsAnyTableElement(parent) ||
                              HTMLEditUtils::IsTableCellOrCaption(*parent);
    }
  }

  if (createEmptyDivElement) {
    if (IsSelectionRangeContainerNotContent()) {
      NS_WARNING("Mutaiton event listener might have changed the selection");
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
    EditActionResult result =
        AlignContentsAtSelectionWithEmptyDivElement(aAlignType);
    NS_WARNING_ASSERTION(
        result.Succeeded(),
        "HTMLEditor::AlignContentsAtSelectionWithEmptyDivElement() failed");
    if (result.Handled()) {
      restoreSelectionLater.Abort();
    }
    return rv;
  }

  rv = AlignNodesAndDescendants(arrayOfContents, aAlignType);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::AlignNodesAndDescendants() failed");
  return rv;
}

EditActionResult HTMLEditor::AlignContentsAtSelectionWithEmptyDivElement(
    const nsAString& aAlignType) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  EditorDOMPoint atStartOfSelection(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  SplitNodeResult splitNodeResult = MaybeSplitAncestorsForInsertWithTransaction(
      *nsGkAtoms::div, atStartOfSelection);
  if (splitNodeResult.Failed()) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms::"
        "div) failed");
    return EditActionResult(splitNodeResult.Rv());
  }

  EditorDOMPoint pointToInsertDiv(splitNodeResult.SplitPoint());

  // Consume a trailing br, if any.  This is to keep an alignment from
  // creating extra lines, if possible.
  if (nsCOMPtr<nsIContent> maybeBRContent =
          GetNextEditableHTMLNodeInBlock(splitNodeResult.SplitPoint())) {
    if (maybeBRContent->IsHTMLElement(nsGkAtoms::br) &&
        pointToInsertDiv.GetChild()) {
      // Making use of html structure... if next node after where we are
      // putting our div is not a block, then the br we found is in same block
      // we are, so it's safe to consume it.
      if (nsIContent* nextEditableSibling =
              GetNextHTMLSibling(pointToInsertDiv.GetChild())) {
        if (!HTMLEditUtils::IsBlockElement(*nextEditableSibling)) {
          AutoEditorDOMPointChildInvalidator lockOffset(pointToInsertDiv);
          nsresult rv = DeleteNodeWithTransaction(*maybeBRContent);
          if (NS_WARN_IF(Destroyed())) {
            return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
          }
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
            return EditActionResult(rv);
          }
        }
      }
    }
  }

  RefPtr<Element> divElement =
      CreateNodeWithTransaction(*nsGkAtoms::div, pointToInsertDiv);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (!divElement) {
    NS_WARNING("EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
    return EditActionResult(NS_ERROR_FAILURE);
  }
  // Remember our new block for postprocessing
  TopLevelEditSubActionDataRef().mNewBlockElement = divElement;
  // Set up the alignment on the div, using HTML or CSS
  nsresult rv = SetBlockElementAlign(*divElement, aAlignType,
                                     EditTarget::OnlyDescendantsExceptTable);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::SetBlockElementAlign(EditTarget::"
        "OnlyDescendantsExceptTable) failed");
    return EditActionResult(rv);
  }
  // Put in a padding <br> element for empty last line so that it won't get
  // deleted.
  CreateElementResult createPaddingBRResult =
      InsertPaddingBRElementForEmptyLastLineWithTransaction(
          EditorDOMPoint(divElement, 0));
  if (createPaddingBRResult.Failed()) {
    NS_WARNING(
        "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
        "failed");
    return EditActionResult(createPaddingBRResult.Rv());
  }
  rv = CollapseSelectionToStartOf(*divElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionToStartOf() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::AlignNodesAndDescendants(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    const nsAString& aAlignType) {
  // Detect all the transitions in the array, where a transition means that
  // adjacent nodes in the array don't have the same parent.
  AutoTArray<bool, 64> transitionList;
  HTMLEditor::MakeTransitionList(aArrayOfContents, transitionList);

  // Okay, now go through all the nodes and give them an align attrib or put
  // them in a div, or whatever is appropriate.  Woohoo!

  RefPtr<Element> createdDivElement;
  bool useCSS = IsCSSEnabled();
  int32_t indexOfTransitionList = -1;
  for (OwningNonNull<nsIContent>& content : aArrayOfContents) {
    ++indexOfTransitionList;

    // Ignore all non-editable nodes.  Leave them be.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    // The node is a table element, an hr, a paragraph, a div or a section
    // header; in HTML 4, it can directly carry the ALIGN attribute and we
    // don't need to nest it, just set the alignment.  In CSS, assign the
    // corresponding CSS styles in SetBlockElementAlign().
    if (HTMLEditUtils::SupportsAlignAttr(content)) {
      nsresult rv =
          SetBlockElementAlign(MOZ_KnownLive(*content->AsElement()), aAlignType,
                               EditTarget::NodeAndDescendantsExceptTable);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::SetBlockElementAlign(EditTarget::"
            "NodeAndDescendantsExceptTable) failed");
        return rv;
      }
      // Clear out createdDivElement so that we don't put nodes after this one
      // into it
      createdDivElement = nullptr;
      continue;
    }

    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Skip insignificant formatting text nodes to prevent unnecessary
    // structure splitting!
    if (content->IsText() &&
        ((HTMLEditUtils::IsAnyTableElement(atContent.GetContainer()) &&
          !HTMLEditUtils::IsTableCellOrCaption(*atContent.GetContainer())) ||
         HTMLEditUtils::IsAnyListElement(atContent.GetContainer()) ||
         IsEmptyNode(*content))) {
      continue;
    }

    // If it's a list item, or a list inside a list, forget any "current" div,
    // and instead put divs inside the appropriate block (td, li, etc.)
    if (HTMLEditUtils::IsListItem(content) ||
        HTMLEditUtils::IsAnyListElement(content)) {
      Element* listOrListItemElement = content->AsElement();
      AutoEditorDOMPointOffsetInvalidator lockChild(atContent);
      // MOZ_KnownLive(*listOrListItemElement): An element of aArrayOfContents
      // which is array of OwningNonNull.
      nsresult rv = RemoveAlignFromDescendants(
          MOZ_KnownLive(*listOrListItemElement), aAlignType,
          EditTarget::OnlyDescendantsExceptTable);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
            "OnlyDescendantsExceptTable) failed");
        return rv;
      }

      if (useCSS) {
        if (nsStyledElement* styledListOrListItemElement =
                nsStyledElement::FromNode(listOrListItemElement)) {
          // MOZ_KnownLive(*styledListOrListItemElement): An element of
          // aArrayOfContents which is array of OwningNonNull.
          Result<int32_t, nsresult> result =
              mCSSEditUtils->SetCSSEquivalentToHTMLStyleWithTransaction(
                  MOZ_KnownLive(*styledListOrListItemElement), nullptr,
                  nsGkAtoms::align, &aAlignType);
          if (result.isErr()) {
            if (result.inspectErr() == NS_ERROR_EDITOR_DESTROYED) {
              NS_WARNING(
                  "CSSEditUtils::SetCSSEquivalentToHTMLStyleWithTransaction("
                  "nsGkAtoms::align) destroyed the editor");
              return NS_ERROR_EDITOR_DESTROYED;
            }
            NS_WARNING(
                "CSSEditUtils::SetCSSEquivalentToHTMLStyleWithTransaction("
                "nsGkAtoms::align) failed, but ignored");
          }
        }
        createdDivElement = nullptr;
        continue;
      }

      if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
        // If we don't use CSS, add a content to list element: they have to
        // be inside another list, i.e., >= second level of nesting.
        // XXX AlignContentsInAllTableCellsAndListItems() handles only list
        //     item elements and table cells.  Is it intentional?  Why don't
        //     we need to align contents in other type blocks?
        // MOZ_KnownLive(*listOrListItemElement): An element of aArrayOfContents
        // which is array of OwningNonNull.
        nsresult rv = AlignContentsInAllTableCellsAndListItems(
            MOZ_KnownLive(*listOrListItemElement), aAlignType);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::AlignContentsInAllTableCellsAndListItems() failed");
          return rv;
        }
        createdDivElement = nullptr;
        continue;
      }

      // Clear out createdDivElement so that we don't put nodes after this one
      // into it
    }

    // Need to make a div to put things in if we haven't already, or if this
    // node doesn't go in div we used earlier.
    if (!createdDivElement || transitionList[indexOfTransitionList]) {
      // First, check that our element can contain a div.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::div)) {
        // XXX Why do we return NS_OK here rather than returning error or
        //     doing continue?
        return NS_OK;
      }

      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::div,
                                                      atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms:"
            ":div) failed");
        return splitNodeResult.Rv();
      }
      createdDivElement = CreateNodeWithTransaction(
          *nsGkAtoms::div, splitNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!createdDivElement) {
        NS_WARNING(
            "EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
        return NS_ERROR_FAILURE;
      }
      // Remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = createdDivElement;
      // Set up the alignment on the div
      nsresult rv =
          SetBlockElementAlign(*createdDivElement, aAlignType,
                               EditTarget::OnlyDescendantsExceptTable);
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::SetBlockElementAlign(EditTarget::"
                           "OnlyDescendantsExceptTable) failed, but ignored");
    }

    // Tuck the node into the end of the active div
    //
    // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it alive.
    nsresult rv = MoveNodeToEndWithTransaction(MOZ_KnownLive(content),
                                               *createdDivElement);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
  }

  return NS_OK;
}

nsresult HTMLEditor::AlignContentsInAllTableCellsAndListItems(
    Element& aElement, const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // Gather list of table cells or list items
  AutoTArray<OwningNonNull<Element>, 64> arrayOfTableCellsAndListItems;
  DOMIterator iter(aElement);
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void*) -> bool {
        MOZ_ASSERT(Element::FromNode(&aNode));
        return HTMLEditUtils::IsTableCell(&aNode) ||
               HTMLEditUtils::IsListItem(&aNode);
      },
      arrayOfTableCellsAndListItems);

  // Now that we have the list, align their contents as requested
  for (auto& tableCellOrListItemElement : arrayOfTableCellsAndListItems) {
    // MOZ_KnownLive because 'arrayOfTableCellsAndListItems' is guaranteed to
    // keep it alive.
    nsresult rv = AlignBlockContentsWithDivElement(
        MOZ_KnownLive(tableCellOrListItemElement), aAlignType);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::AlignBlockContentsWithDivElement() failed");
      return rv;
    }
  }

  return NS_OK;
}

nsresult HTMLEditor::AlignBlockContentsWithDivElement(
    Element& aBlockElement, const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // XXX I don't understand why we should NOT align non-editable children
  //     with modifying EDITABLE `<div>` element.
  nsCOMPtr<nsIContent> firstEditableContent =
      GetFirstEditableChild(aBlockElement);
  if (!firstEditableContent) {
    // This block has no editable content, nothing to align.
    return NS_OK;
  }

  // If there is only one editable content and it's a `<div>` element,
  // just set `align` attribute of it.
  nsCOMPtr<nsIContent> lastEditableContent =
      GetLastEditableChild(aBlockElement);
  if (firstEditableContent == lastEditableContent &&
      firstEditableContent->IsHTMLElement(nsGkAtoms::div)) {
    nsresult rv = SetAttributeOrEquivalent(
        MOZ_KnownLive(firstEditableContent->AsElement()), nsGkAtoms::align,
        aAlignType, false);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
    return rv;
  }

  // Otherwise, we need to insert a `<div>` element to set `align` attribute.
  // XXX Don't insert the new `<div>` element until we set `align` attribute
  //     for avoiding running mutation event listeners.
  RefPtr<Element> divElement = CreateNodeWithTransaction(
      *nsGkAtoms::div, EditorDOMPoint(&aBlockElement, 0));
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (!divElement) {
    NS_WARNING("EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv =
      SetAttributeOrEquivalent(divElement, nsGkAtoms::align, aAlignType, false);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
    return rv;
  }
  // XXX This is tricky and does not work with mutation event listeners.
  //     But I'm not sure what we should do if new content is inserted.
  //     Anyway, I don't think that we should move editable contents
  //     over non-editable contents.  Chrome does no do that.
  while (lastEditableContent && (lastEditableContent != divElement)) {
    nsresult rv = MoveNodeWithTransaction(*lastEditableContent,
                                          EditorDOMPoint(divElement, 0));
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return rv;
    }
    lastEditableContent = GetLastEditableChild(aBlockElement);
  }
  return NS_OK;
}

Element* HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    ScanEmptyBlockInclusiveAncestor(const HTMLEditor& aHTMLEditor,
                                    nsIContent& aStartContent,
                                    Element& aEditingHostElement) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  // If the editing host is an inline element, bail out early.
  if (HTMLEditUtils::IsInlineElement(aEditingHostElement)) {
    return nullptr;
  }

  // If we are inside an empty block, delete it.  Note: do NOT delete table
  // elements this way.
  Element* blockElement =
      HTMLEditUtils::GetInclusiveAncestorBlockElement(aStartContent);
  if (!blockElement) {
    return nullptr;
  }
  while (blockElement && blockElement != &aEditingHostElement &&
         !HTMLEditUtils::IsAnyTableElement(blockElement) &&
         aHTMLEditor.IsEmptyNode(*blockElement, true, false)) {
    mEmptyInclusiveAncestorBlockElement = blockElement;
    blockElement = HTMLEditUtils::GetAncestorBlockElement(
        *mEmptyInclusiveAncestorBlockElement);
  }
  if (!mEmptyInclusiveAncestorBlockElement) {
    return nullptr;
  }

  // XXX Because of not checking whether found block element is editable
  //     in the above loop, empty ediable block element may be overwritten
  //     with empty non-editable clock element.  Therefore, we fail to
  //     remove the found empty nodes.
  if (NS_WARN_IF(!mEmptyInclusiveAncestorBlockElement->IsEditable()) ||
      NS_WARN_IF(!mEmptyInclusiveAncestorBlockElement->GetParentElement())) {
    mEmptyInclusiveAncestorBlockElement = nullptr;
  }
  return mEmptyInclusiveAncestorBlockElement;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    ComputeTargetRanges(const HTMLEditor& aHTMLEditor,
                        nsIEditor::EDirection aDirectionAndAmount,
                        const Element& aEditingHost,
                        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);

  // We'll delete `mEmptyInclusiveAncestorBlockElement` node from the tree, but
  // we should return the range from start/end of next/previous editable content
  // to end/start of the element for compatiblity with the other browsers.
  switch (aDirectionAndAmount) {
    case nsIEditor::eNone:
      break;
    case nsIEditor::ePrevious:
    case nsIEditor::ePreviousWord:
    case nsIEditor::eToBeginningOfLine: {
      EditorRawDOMPoint startPoint =
          HTMLEditUtils::GetPreviousEditablePoint<EditorRawDOMPoint>(
              *mEmptyInclusiveAncestorBlockElement, &aEditingHost,
              // In this case, we don't join block elements so that we won't
              // delete invisible trailing whitespaces in the previous element.
              InvisibleWhiteSpaces::Preserve,
              // In this case, we won't join table cells so that we should
              // get a range which is in a table cell even if it's in a
              // table.
              TableBoundary::NoCrossAnyTableElement);
      if (!startPoint.IsSet()) {
        NS_WARNING(
            "HTMLEditUtils::GetPreviousEditablePoint() didn't return a valid "
            "point");
        return NS_ERROR_FAILURE;
      }
      nsresult rv = aRangesToDelete.SetStartAndEnd(
          startPoint,
          EditorRawDOMPoint::AtEndOf(mEmptyInclusiveAncestorBlockElement));
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::SetStartAndEnd() failed");
      return rv;
    }
    case nsIEditor::eNext:
    case nsIEditor::eNextWord:
    case nsIEditor::eToEndOfLine: {
      EditorRawDOMPoint endPoint =
          HTMLEditUtils::GetNextEditablePoint<EditorRawDOMPoint>(
              *mEmptyInclusiveAncestorBlockElement, &aEditingHost,
              // In this case, we don't join block elements so that we won't
              // delete invisible trailing whitespaces in the next element.
              InvisibleWhiteSpaces::Preserve,
              // In this case, we won't join table cells so that we should
              // get a range which is in a table cell even if it's in a
              // table.
              TableBoundary::NoCrossAnyTableElement);
      if (!endPoint.IsSet()) {
        NS_WARNING(
            "HTMLEditUtils::GetNextEditablePoint() didn't return a valid "
            "point");
        return NS_ERROR_FAILURE;
      }
      nsresult rv = aRangesToDelete.SetStartAndEnd(
          EditorRawDOMPoint(mEmptyInclusiveAncestorBlockElement, 0), endPoint);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::SetStartAndEnd() failed");
      return rv;
    }
    default:
      MOZ_ASSERT_UNREACHABLE("Handle the nsIEditor::EDirection value");
      break;
  }
  // No direction, let's select the element to be deleted.
  nsresult rv =
      aRangesToDelete.SelectNode(*mEmptyInclusiveAncestorBlockElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::SelectNode() failed");
  return rv;
}

Result<RefPtr<Element>, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    MaybeInsertBRElementBeforeEmptyListItemElement(HTMLEditor& aHTMLEditor) {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(HTMLEditUtils::IsListItem(mEmptyInclusiveAncestorBlockElement));

  // If the found empty block is a list item element and its grand parent
  // (i.e., parent of list element) is NOT a list element, insert <br>
  // element before the list element which has the empty list item.
  // This odd list structure may occur if `Document.execCommand("indent")`
  // is performed for list items.
  // XXX Chrome does not remove empty list elements when last content in
  //     last list item is deleted.  We should follow it since current
  //     behavior is annoying when you type new list item with selecting
  //     all list items.
  if (!aHTMLEditor.IsFirstEditableChild(mEmptyInclusiveAncestorBlockElement)) {
    return RefPtr<Element>();
  }

  EditorDOMPoint atParentOfEmptyListItem(
      mEmptyInclusiveAncestorBlockElement->GetParentElement());
  if (NS_WARN_IF(!atParentOfEmptyListItem.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }
  if (HTMLEditUtils::IsAnyListElement(atParentOfEmptyListItem.GetContainer())) {
    return RefPtr<Element>();
  }
  RefPtr<Element> brElement =
      aHTMLEditor.InsertBRElementWithTransaction(atParentOfEmptyListItem);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  if (!brElement) {
    NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
    return Err(NS_ERROR_FAILURE);
  }
  return brElement;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoEmptyBlockAncestorDeleter::GetNewCaretPoisition(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount) const {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  switch (aDirectionAndAmount) {
    case nsIEditor::eNext:
    case nsIEditor::eNextWord:
    case nsIEditor::eToEndOfLine: {
      // Collapse Selection to next node of after empty block element
      // if there is.  Otherwise, to just after the empty block.
      EditorDOMPoint afterEmptyBlock(
          EditorRawDOMPoint::After(mEmptyInclusiveAncestorBlockElement));
      MOZ_ASSERT(afterEmptyBlock.IsSet());
      if (nsIContent* nextContentOfEmptyBlock =
              aHTMLEditor.GetNextNode(afterEmptyBlock)) {
        EditorDOMPoint pt = aHTMLEditor.GetGoodCaretPointFor(
            *nextContentOfEmptyBlock, aDirectionAndAmount);
        if (!pt.IsSet()) {
          NS_WARNING("HTMLEditor::GetGoodCaretPointFor() failed");
          return Err(NS_ERROR_FAILURE);
        }
        return pt;
      }
      if (NS_WARN_IF(!afterEmptyBlock.IsSet())) {
        return Err(NS_ERROR_FAILURE);
      }
      return afterEmptyBlock;
    }
    case nsIEditor::ePrevious:
    case nsIEditor::ePreviousWord:
    case nsIEditor::eToBeginningOfLine: {
      // Collapse Selection to previous editable node of the empty block
      // if there is.  Otherwise, to after the empty block.
      EditorRawDOMPoint atEmptyBlock(mEmptyInclusiveAncestorBlockElement);
      if (nsIContent* previousContentOfEmptyBlock =
              aHTMLEditor.GetPreviousEditableNode(atEmptyBlock)) {
        EditorDOMPoint pt = aHTMLEditor.GetGoodCaretPointFor(
            *previousContentOfEmptyBlock, aDirectionAndAmount);
        if (!pt.IsSet()) {
          NS_WARNING("HTMLEditor::GetGoodCaretPointFor() failed");
          return Err(NS_ERROR_FAILURE);
        }
        return pt;
      }
      EditorDOMPoint afterEmptyBlock(
          EditorRawDOMPoint::After(*mEmptyInclusiveAncestorBlockElement));
      if (NS_WARN_IF(!afterEmptyBlock.IsSet())) {
        return Err(NS_ERROR_FAILURE);
      }
      return afterEmptyBlock;
    }
    case nsIEditor::eNone:
      return EditorDOMPoint();
    default:
      MOZ_CRASH(
          "AutoEmptyBlockAncestorDeleter doesn't support this action yet");
      return EditorDOMPoint();
  }
}

EditActionResult
HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::Run(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount) {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (HTMLEditUtils::IsListItem(mEmptyInclusiveAncestorBlockElement)) {
    Result<RefPtr<Element>, nsresult> result =
        MaybeInsertBRElementBeforeEmptyListItemElement(aHTMLEditor);
    if (result.isErr()) {
      NS_WARNING(
          "AutoEmptyBlockAncestorDeleter::"
          "MaybeInsertBRElementBeforeEmptyListItemElement() failed");
      return EditActionResult(result.inspectErr());
    }
    // If a `<br>` element is inserted, caret should be moved to after it.
    if (RefPtr<Element> brElement = result.unwrap()) {
      nsresult rv =
          aHTMLEditor.CollapseSelectionTo(EditorRawDOMPoint(brElement));
      if (NS_FAILED(rv)) {
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "HTMLEditor::CollapseSelectionTo() failed");
        return EditActionResult(rv);
      }
    }
  } else {
    Result<EditorDOMPoint, nsresult> result =
        GetNewCaretPoisition(aHTMLEditor, aDirectionAndAmount);
    if (result.isErr()) {
      NS_WARNING(
          "AutoEmptyBlockAncestorDeleter::GetNewCaretPoisition() failed");
      return EditActionResult(result.inspectErr());
    }
    if (result.inspect().IsSet()) {
      nsresult rv = aHTMLEditor.CollapseSelectionTo(result.inspect());
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::CollapseSelectionTo() failed");
        return EditActionResult(rv);
      }
    }
  }
  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(
      MOZ_KnownLive(*mEmptyInclusiveAncestorBlockElement));
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
    return EditActionResult(rv);
  }
  return EditActionHandled();
}

size_t HTMLEditor::CollectChildren(
    nsINode& aNode, nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
    size_t aIndexToInsertChildren, CollectListChildren aCollectListChildren,
    CollectTableChildren aCollectTableChildren,
    CollectNonEditableNodes aCollectNonEditableNodes) const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  size_t numberOfFoundChildren = 0;
  for (nsIContent* content = GetFirstEditableChild(aNode); content;
       content = content->GetNextSibling()) {
    if ((aCollectListChildren == CollectListChildren::Yes &&
         (HTMLEditUtils::IsAnyListElement(content) ||
          HTMLEditUtils::IsListItem(content))) ||
        (aCollectTableChildren == CollectTableChildren::Yes &&
         HTMLEditUtils::IsAnyTableElement(content))) {
      numberOfFoundChildren += CollectChildren(
          *content, aOutArrayOfContents,
          aIndexToInsertChildren + numberOfFoundChildren, aCollectListChildren,
          aCollectTableChildren, aCollectNonEditableNodes);
    } else if (aCollectNonEditableNodes == CollectNonEditableNodes::Yes ||
               EditorUtils::IsEditableContent(*content, EditorType::HTML)) {
      aOutArrayOfContents.InsertElementAt(
          aIndexToInsertChildren + numberOfFoundChildren++, *content);
    }
  }
  return numberOfFoundChildren;
}

bool HTMLEditor::AutoDeleteRangesHandler::ExtendRangeToIncludeInvisibleNodes(
    const HTMLEditor& aHTMLEditor, const nsFrameSelection* aFrameSelection,
    nsRange& aRange) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRange.Collapsed());
  MOZ_ASSERT(aRange.IsPositioned());
  MOZ_ASSERT(!aRange.IsInSelection());

  EditorRawDOMPoint atStart(aRange.StartRef());
  EditorRawDOMPoint atEnd(aRange.EndRef());

  if (NS_WARN_IF(!aRange.GetClosestCommonInclusiveAncestor()->IsContent())) {
    return false;
  }

  // Find current selection common block parent
  Element* commonAncestorBlock =
      HTMLEditUtils::GetInclusiveAncestorBlockElement(
          *aRange.GetClosestCommonInclusiveAncestor()->AsContent());
  if (NS_WARN_IF(!commonAncestorBlock)) {
    return false;
  }

  // Set up for loops and cache our root element
  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return false;
  }

  // Find previous visible things before start of selection
  if (atStart.GetContainer() != commonAncestorBlock &&
      atStart.GetContainer() != editingHost) {
    for (;;) {
      WSScanResult backwardScanFromStartResult =
          WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundary(aHTMLEditor,
                                                               atStart);
      if (!backwardScanFromStartResult.ReachedCurrentBlockBoundary()) {
        break;
      }
      MOZ_ASSERT(backwardScanFromStartResult.GetContent() ==
                 WSRunScanner(aHTMLEditor, atStart).GetStartReasonContent());
      // We want to keep looking up.  But stop if we are crossing table
      // element boundaries, or if we hit the root.
      if (HTMLEditUtils::IsAnyTableElement(
              backwardScanFromStartResult.GetContent()) ||
          backwardScanFromStartResult.GetContent() == commonAncestorBlock ||
          backwardScanFromStartResult.GetContent() == editingHost) {
        break;
      }
      atStart = backwardScanFromStartResult.PointAtContent();
    }
    if (aFrameSelection &&
        !aFrameSelection->IsValidSelectionPoint(atStart.GetContainer())) {
      NS_WARNING("Computed start container was out of selection limiter");
      return false;
    }
  }

  // Expand selection endpoint only if we don't pass an invisible `<br>`, or if
  // we really needed to pass that `<br>` (i.e., its block is now totally
  // selected).

  // Find next visible things after end of selection
  if (atEnd.GetContainer() != commonAncestorBlock &&
      atEnd.GetContainer() != editingHost) {
    EditorDOMPoint atFirstInvisibleBRElement;
    for (;;) {
      WSRunScanner wsScannerAtEnd(aHTMLEditor, atEnd);
      WSScanResult forwardScanFromEndResult =
          wsScannerAtEnd.ScanNextVisibleNodeOrBlockBoundaryFrom(atEnd);
      if (forwardScanFromEndResult.ReachedBRElement()) {
        // XXX In my understanding, this is odd.  The end reason may not be
        //     same as the reached <br> element because the equality is
        //     guaranteed only when ReachedCurrentBlockBoundary() returns true.
        //     However, looks like that this code assumes that
        //     GetEndReasonContent() returns the (or a) <br> element.
        NS_ASSERTION(wsScannerAtEnd.GetEndReasonContent() ==
                         forwardScanFromEndResult.BRElementPtr(),
                     "End reason is not the reached <br> element");
        if (aHTMLEditor.IsVisibleBRElement(
                wsScannerAtEnd.GetEndReasonContent())) {
          break;
        }
        if (!atFirstInvisibleBRElement.IsSet()) {
          atFirstInvisibleBRElement = atEnd;
        }
        atEnd.SetAfter(wsScannerAtEnd.GetEndReasonContent());
        continue;
      }

      if (forwardScanFromEndResult.ReachedCurrentBlockBoundary()) {
        MOZ_ASSERT(forwardScanFromEndResult.GetContent() ==
                   wsScannerAtEnd.GetEndReasonContent());
        // We want to keep looking up.  But stop if we are crossing table
        // element boundaries, or if we hit the root.
        if (HTMLEditUtils::IsAnyTableElement(
                forwardScanFromEndResult.GetContent()) ||
            forwardScanFromEndResult.GetContent() == commonAncestorBlock ||
            forwardScanFromEndResult.GetContent() == editingHost) {
          break;
        }
        atEnd = forwardScanFromEndResult.PointAfterContent();
        continue;
      }

      break;
    }

    if (aFrameSelection &&
        !aFrameSelection->IsValidSelectionPoint(atEnd.GetContainer())) {
      NS_WARNING("Computed end container was out of selection limiter");
      return false;
    }

    if (atFirstInvisibleBRElement.IsInContentNode()) {
      // Find block node containing invisible `<br>` element.
      if (RefPtr<Element> brElementParent =
              HTMLEditUtils::GetInclusiveAncestorBlockElement(
                  *atFirstInvisibleBRElement.ContainerAsContent())) {
        EditorRawDOMRange range(atStart, atEnd);
        if (range.Contains(EditorRawDOMPoint(brElementParent))) {
          nsresult rv = aRange.SetStartAndEnd(atStart.ToRawRangeBoundary(),
                                              atEnd.ToRawRangeBoundary());
          if (NS_FAILED(rv)) {
            NS_WARNING("nsRange::SetStartAndEnd() failed to extend the range");
            return false;
          }
          return aRange.IsPositioned() && aRange.StartRef().IsSet() &&
                 aRange.EndRef().IsSet();
        }
        // Otherwise, the new range should end at the invisible `<br>`.
        if (aFrameSelection && !aFrameSelection->IsValidSelectionPoint(
                                   atFirstInvisibleBRElement.GetContainer())) {
          NS_WARNING(
              "Computed end container (`<br>` element) was out of selection "
              "limiter");
          return false;
        }
        atEnd = atFirstInvisibleBRElement;
      }
    }
  }

  // XXX This is unnecessary creation cost for us since we just want to return
  //     the start point and the end point.
  nsresult rv = aRange.SetStartAndEnd(atStart.ToRawRangeBoundary(),
                                      atEnd.ToRawRangeBoundary());
  if (NS_FAILED(rv)) {
    NS_WARNING("nsRange::SetStartAndEnd() failed to extend the range");
    return false;
  }
  return aRange.IsPositioned() && aRange.StartRef().IsSet() &&
         aRange.EndRef().IsSet();
}

nsresult HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // This tweaks selections to be more "natural".
  // Idea here is to adjust edges of selection ranges so that they do not cross
  // breaks or block boundaries unless something editable beyond that boundary
  // is also selected.  This adjustment makes it much easier for the various
  // block operations to determine what nodes to act on.

  // We don't need to mess with cell selections, and we assume multirange
  // selections are those.
  // XXX Why?  Even in <input>, user can select 2 or more ranges.
  if (SelectionRefPtr()->RangeCount() != 1) {
    return NS_OK;
  }

  const RefPtr<nsRange> range = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!range)) {
    return NS_ERROR_FAILURE;
  }

  if (NS_WARN_IF(!range->IsPositioned())) {
    return NS_ERROR_FAILURE;
  }

  const EditorDOMPoint startPoint(range->StartRef());
  if (NS_WARN_IF(!startPoint.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  const EditorDOMPoint endPoint(range->EndRef());
  if (NS_WARN_IF(!endPoint.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // adjusted values default to original values
  EditorDOMPoint newStartPoint(startPoint);
  EditorDOMPoint newEndPoint(endPoint);

  // Is there any intervening visible white-space?  If so we can't push
  // selection past that, it would visibly change meaning of users selection.
  WSRunScanner wsScannerAtEnd(*this, endPoint);
  if (wsScannerAtEnd.ScanPreviousVisibleNodeOrBlockBoundaryFrom(endPoint)
          .ReachedSomething()) {
    // eThisBlock and eOtherBlock conveniently distinguish cases
    // of going "down" into a block and "up" out of a block.
    if (wsScannerAtEnd.StartsFromOtherBlockElement()) {
      // endpoint is just after the close of a block.
      nsIContent* child = HTMLEditUtils::GetLastLeafChild(
          *wsScannerAtEnd.StartReasonOtherBlockElementPtr(),
          ChildBlockBoundary::TreatAsLeaf);
      if (child) {
        newEndPoint.SetAfter(child);
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtEnd.StartsFromCurrentBlockBoundary()) {
      // endpoint is just after start of this block
      nsINode* child = GetPreviousEditableHTMLNode(endPoint);
      if (child) {
        newEndPoint.SetAfter(child);
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtEnd.StartsFromBRElement()) {
      // endpoint is just after break.  lets adjust it to before it.
      newEndPoint.Set(wsScannerAtEnd.StartReasonBRElementPtr());
    }
  }

  // Is there any intervening visible white-space?  If so we can't push
  // selection past that, it would visibly change meaning of users selection.
  WSRunScanner wsScannerAtStart(*this, startPoint);
  if (wsScannerAtStart.ScanNextVisibleNodeOrBlockBoundaryFrom(startPoint)
          .ReachedSomething()) {
    // eThisBlock and eOtherBlock conveniently distinguish cases
    // of going "down" into a block and "up" out of a block.
    if (wsScannerAtStart.EndsByOtherBlockElement()) {
      // startpoint is just before the start of a block.
      nsINode* child = HTMLEditUtils::GetFirstLeafChild(
          *wsScannerAtStart.EndReasonOtherBlockElementPtr(),
          ChildBlockBoundary::TreatAsLeaf);
      if (child) {
        newStartPoint.Set(child);
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtStart.EndsByCurrentBlockBoundary()) {
      // startpoint is just before end of this block
      nsINode* child = GetNextEditableHTMLNode(startPoint);
      if (child) {
        newStartPoint.Set(child);
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtStart.EndsByBRElement()) {
      // startpoint is just before a break.  lets adjust it to after it.
      newStartPoint.SetAfter(wsScannerAtStart.EndReasonBRElementPtr());
    }
  }

  // There is a demented possibility we have to check for.  We might have a very
  // strange selection that is not collapsed and yet does not contain any
  // editable content, and satisfies some of the above conditions that cause
  // tweaking.  In this case we don't want to tweak the selection into a block
  // it was never in, etc.  There are a variety of strategies one might use to
  // try to detect these cases, but I think the most straightforward is to see
  // if the adjusted locations "cross" the old values: i.e., new end before old
  // start, or new start after old end.  If so then just leave things alone.

  Maybe<int32_t> comp = nsContentUtils::ComparePoints(
      startPoint.ToRawRangeBoundary(), newEndPoint.ToRawRangeBoundary());

  if (NS_WARN_IF(!comp)) {
    return NS_ERROR_FAILURE;
  }

  if (*comp == 1) {
    return NS_OK;  // New end before old start.
  }

  comp = nsContentUtils::ComparePoints(newStartPoint.ToRawRangeBoundary(),
                                       endPoint.ToRawRangeBoundary());

  if (NS_WARN_IF(!comp)) {
    return NS_ERROR_FAILURE;
  }

  if (*comp == 1) {
    return NS_OK;  // New start after old end.
  }

  // Otherwise set selection to new values.  Note that end point may be prior
  // to start point.  So, we cannot use Selection::SetStartAndEndInLimit() here.
  ErrorResult error;
  MOZ_KnownLive(SelectionRefPtr())
      ->SetBaseAndExtentInLimiter(newStartPoint, newEndPoint, error);
  if (NS_WARN_IF(Destroyed())) {
    error = NS_ERROR_EDITOR_DESTROYED;
  } else if (error.Failed()) {
    NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
  }
  return error.StealNSResult();
}

// static
template <typename PT, typename RT>
EditorDOMPoint HTMLEditor::GetWhiteSpaceEndPoint(
    const RangeBoundaryBase<PT, RT>& aPoint, ScanDirection aScanDirection) {
  if (NS_WARN_IF(!aPoint.IsSet()) ||
      NS_WARN_IF(!aPoint.Container()->IsContent())) {
    return EditorDOMPoint();
  }

  bool isSpace = false, isNBSP = false;
  nsIContent* newContent = aPoint.Container()->AsContent();
  int32_t newOffset = *aPoint.Offset(
      RangeBoundaryBase<PT, RT>::OffsetFilter::kValidOrInvalidOffsets);
  while (newContent) {
    int32_t offset = -1;
    nsCOMPtr<nsIContent> content;
    if (aScanDirection == ScanDirection::Backward) {
      HTMLEditor::IsPrevCharInNodeWhiteSpace(newContent, newOffset, &isSpace,
                                             &isNBSP, getter_AddRefs(content),
                                             &offset);
    } else {
      HTMLEditor::IsNextCharInNodeWhiteSpace(newContent, newOffset, &isSpace,
                                             &isNBSP, getter_AddRefs(content),
                                             &offset);
    }
    if (!isSpace && !isNBSP) {
      break;
    }
    newContent = content;
    newOffset = offset;
  }
  return EditorDOMPoint(newContent, newOffset);
}

template <typename PT, typename RT>
EditorDOMPoint HTMLEditor::GetCurrentHardLineStartPoint(
    const RangeBoundaryBase<PT, RT>& aPoint,
    EditSubAction aEditSubAction) const {
  if (NS_WARN_IF(!aPoint.IsSet())) {
    return EditorDOMPoint();
  }

  EditorDOMPoint point(aPoint);
  // Start scanning from the container node if aPoint is in a text node.
  // XXX Perhaps, IsInDataNode() must be expected.
  if (point.IsInTextNode()) {
    if (!point.GetContainer()->GetParentNode()) {
      // Okay, can't promote any further
      // XXX Why don't we return start of the text node?
      return point;
    }
    point.Set(point.GetContainer());
  }

  // Look back through any further inline nodes that aren't across a <br>
  // from us, and that are enclosed in the same block.
  // I.e., looking for start of current hard line.
  for (nsIContent* previousEditableContent =
           GetPreviousEditableHTMLNodeInBlock(point);
       previousEditableContent && previousEditableContent->GetParentNode() &&
       !IsVisibleBRElement(previousEditableContent) &&
       !HTMLEditUtils::IsBlockElement(*previousEditableContent);
       previousEditableContent = GetPreviousEditableHTMLNodeInBlock(point)) {
    point.Set(previousEditableContent);
    // XXX Why don't we check \n in pre-formated text node here?
  }

  // Finding the real start for this point unless current line starts after
  // <br> element.  Look up the tree for as long as we are the first node in
  // the container (typically, start of nearest block ancestor), and as long
  // as we haven't hit the body node.
  for (nsIContent* nearContent = GetPreviousEditableHTMLNodeInBlock(point);
       !nearContent && !point.IsContainerHTMLElement(nsGkAtoms::body) &&
       point.GetContainer()->GetParentNode();
       nearContent = GetPreviousEditableHTMLNodeInBlock(point)) {
    // Don't keep looking up if we have found a blockquote element to act on
    // when we handle outdent.
    // XXX Sounds like this is hacky.  If possible, it should be check in
    //     outdent handler for consistency between edit sub-actions.
    //     We should check Chromium's behavior of outdent when Selection
    //     starts from `<blockquote>` and starts from first child of
    //     `<blockquote>`.
    if (aEditSubAction == EditSubAction::eOutdent &&
        point.IsContainerHTMLElement(nsGkAtoms::blockquote)) {
      break;
    }

    // Don't walk past the editable section. Note that we need to check
    // before walking up to a parent because we need to return the parent
    // object, so the parent itself might not be in the editable area, but
    // it's OK if we're not performing a block-level action.
    // XXX Here is too slow.  Let's cache active editing host first, then,
    //     compair with container of the point when we climb up the tree.
    bool blockLevelAction =
        aEditSubAction == EditSubAction::eIndent ||
        aEditSubAction == EditSubAction::eOutdent ||
        aEditSubAction == EditSubAction::eSetOrClearAlignment ||
        aEditSubAction == EditSubAction::eCreateOrRemoveBlock;
    if (!IsDescendantOfEditorRoot(point.GetContainer()->GetParentNode()) &&
        (blockLevelAction || !IsDescendantOfEditorRoot(point.GetContainer()))) {
      break;
    }

    point.Set(point.GetContainer());
  }
  return point;
}

template <typename PT, typename RT>
EditorDOMPoint HTMLEditor::GetCurrentHardLineEndPoint(
    const RangeBoundaryBase<PT, RT>& aPoint) const {
  if (NS_WARN_IF(!aPoint.IsSet())) {
    return EditorDOMPoint();
  }

  EditorDOMPoint point(aPoint);
  // Start scanning from the container node if aPoint is in a text node.
  // XXX Perhaps, IsInDataNode() must be expected.
  if (point.IsInTextNode()) {
    if (!point.GetContainer()->GetParentNode()) {
      // Okay, can't promote any further
      // XXX Why don't we return end of the text node?
      return point;
    }
    // want to be after the text node
    point.SetAfter(point.GetContainer());
    NS_WARNING_ASSERTION(point.IsSet(), "Failed to set to after the text node");
  }

  // Look ahead through any further inline nodes that aren't across a <br> from
  // us, and that are enclosed in the same block.
  // XXX Currently, we stop block-extending when finding visible <br> element.
  //     This might be different from "block-extend" of execCommand spec.
  //     However, the spec is really unclear.
  // XXX Probably, scanning only editable nodes is wrong for
  //     EditSubAction::eCreateOrRemoveBlock because it might be better to wrap
  //     existing inline elements even if it's non-editable.  For example,
  //     following examples with insertParagraph causes different result:
  //     * <div contenteditable>foo[]<b contenteditable="false">bar</b></div>
  //     * <div contenteditable>foo[]<b>bar</b></div>
  //     * <div contenteditable>foo[]<b contenteditable="false">bar</b>baz</div>
  //     Only in the first case, after the caret position isn't wrapped with
  //     new <div> element.
  for (nsIContent* nextEditableContent = GetNextEditableHTMLNodeInBlock(point);
       nextEditableContent &&
       !HTMLEditUtils::IsBlockElement(*nextEditableContent) &&
       nextEditableContent->GetParent();
       nextEditableContent = GetNextEditableHTMLNodeInBlock(point)) {
    point.SetAfter(nextEditableContent);
    if (NS_WARN_IF(!point.IsSet())) {
      break;
    }
    if (IsVisibleBRElement(nextEditableContent)) {
      break;
    }

    // Check for newlines in pre-formatted text nodes.
    if (nextEditableContent->IsText() &&
        EditorUtils::IsContentPreformatted(*nextEditableContent)) {
      nsAutoString textContent;
      nextEditableContent->GetAsText()->GetData(textContent);
      int32_t newlinePos = textContent.FindChar(nsCRT::LF);
      if (newlinePos >= 0) {
        if (static_cast<uint32_t>(newlinePos) + 1 == textContent.Length()) {
          // No need for special processing if the newline is at the end.
          break;
        }
        return EditorDOMPoint(nextEditableContent, newlinePos + 1);
      }
    }
  }

  // Finding the real end for this point unless current line ends with a <br>
  // element.  Look up the tree for as long as we are the last node in the
  // container (typically, block node), and as long as we haven't hit the body
  // node.
  for (nsIContent* nearContent = GetNextEditableHTMLNodeInBlock(point);
       !nearContent && !point.IsContainerHTMLElement(nsGkAtoms::body) &&
       point.GetContainer()->GetParentNode();
       nearContent = GetNextEditableHTMLNodeInBlock(point)) {
    // Don't walk past the editable section. Note that we need to check before
    // walking up to a parent because we need to return the parent object, so
    // the parent itself might not be in the editable area, but it's OK.
    // XXX Maybe returning parent of editing host is really error prone since
    //     everybody need to check whether the end point is in editing host
    //     when they touch there.
    // XXX Here is too slow.  Let's cache active editing host first, then,
    //     compair with container of the point when we climb up the tree.
    if (!IsDescendantOfEditorRoot(point.GetContainer()) &&
        !IsDescendantOfEditorRoot(point.GetContainer()->GetParentNode())) {
      break;
    }

    point.SetAfter(point.GetContainer());
    if (NS_WARN_IF(!point.IsSet())) {
      break;
    }
  }
  return point;
}

void HTMLEditor::GetSelectionRangesExtendedToIncludeAdjuscentWhiteSpaces(
    nsTArray<RefPtr<nsRange>>& aOutArrayOfRanges) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aOutArrayOfRanges.IsEmpty());

  aOutArrayOfRanges.SetCapacity(SelectionRefPtr()->RangeCount());
  for (uint32_t i = 0; i < SelectionRefPtr()->RangeCount(); i++) {
    const nsRange* selectionRange = SelectionRefPtr()->GetRangeAt(i);
    MOZ_ASSERT(selectionRange);

    RefPtr<nsRange> extendedRange =
        CreateRangeIncludingAdjuscentWhiteSpaces(*selectionRange);
    if (!extendedRange) {
      extendedRange = selectionRange->CloneRange();
    }

    aOutArrayOfRanges.AppendElement(extendedRange);
  }
}
void HTMLEditor::GetSelectionRangesExtendedToHardLineStartAndEnd(
    nsTArray<RefPtr<nsRange>>& aOutArrayOfRanges,
    EditSubAction aEditSubAction) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aOutArrayOfRanges.IsEmpty());

  aOutArrayOfRanges.SetCapacity(SelectionRefPtr()->RangeCount());
  for (uint32_t i = 0; i < SelectionRefPtr()->RangeCount(); i++) {
    // Make a new adjusted range to represent the appropriate block content.
    // The basic idea is to push out the range endpoints to truly enclose the
    // blocks that we will affect.  This call alters opRange.
    nsRange* selectionRange = SelectionRefPtr()->GetRangeAt(i);
    MOZ_ASSERT(selectionRange);

    RefPtr<nsRange> extendedRange = CreateRangeExtendedToHardLineStartAndEnd(
        *selectionRange, aEditSubAction);
    if (!extendedRange) {
      extendedRange = selectionRange->CloneRange();
    }

    aOutArrayOfRanges.AppendElement(extendedRange);
  }
}

template <typename SPT, typename SRT, typename EPT, typename ERT>
void HTMLEditor::SelectBRElementIfCollapsedInEmptyBlock(
    RangeBoundaryBase<SPT, SRT>& aStartRef,
    RangeBoundaryBase<EPT, ERT>& aEndRef) const {
  // MOOSE major hack:
  // The GetWhiteSpaceEndPoint(), GetCurrentHardLineStartPoint() and
  // GetCurrentHardLineEndPoint() don't really do the right thing for
  // collapsed ranges inside block elements that contain nothing but a solo
  // <br>.  It's easier/ to put a workaround here than to revamp them.  :-(
  // XXX This sounds odd in the cases using `GetWhiteSpaceEndPoint()`.  Don't
  //     we hack something in the edit sub-action handlers?
  if (aStartRef != aEndRef) {
    return;
  }

  if (!aStartRef.Container()->IsContent()) {
    return;
  }

  Element* blockElement = HTMLEditUtils::GetInclusiveAncestorBlockElement(
      *aStartRef.Container()->AsContent());
  if (!blockElement) {
    return;
  }

  Element* editingHost = GetActiveEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return;
  }

  // Make sure we don't go higher than our root element in the content tree
  if (editingHost->IsInclusiveDescendantOf(blockElement)) {
    return;
  }

  if (IsEmptyNode(*blockElement, true, false)) {
    aStartRef = {blockElement, 0u};
    aEndRef = {blockElement, blockElement->Length()};
  }
}

already_AddRefed<nsRange> HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const AbstractRange& aAbstractRange) {
  if (!aAbstractRange.IsPositioned()) {
    return nullptr;
  }
  return CreateRangeIncludingAdjuscentWhiteSpaces(aAbstractRange.StartRef(),
                                                  aAbstractRange.EndRef());
}

template <typename SPT, typename SRT, typename EPT, typename ERT>
already_AddRefed<nsRange> HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const RangeBoundaryBase<SPT, SRT>& aStartRef,
    const RangeBoundaryBase<EPT, ERT>& aEndRef) {
  if (NS_WARN_IF(!aStartRef.IsSet()) || NS_WARN_IF(!aEndRef.IsSet())) {
    return nullptr;
  }

  if (!aStartRef.Container()->IsContent() ||
      !aEndRef.Container()->IsContent()) {
    return nullptr;
  }

  RangeBoundaryBase<SPT, SRT> startRef(aStartRef);
  RangeBoundaryBase<EPT, ERT> endRef(aEndRef);
  SelectBRElementIfCollapsedInEmptyBlock(startRef, endRef);

  // For text actions, we want to look backwards (or forwards, as
  // appropriate) for additional white-space or nbsp's.  We may have to act
  // on these later even though they are outside of the initial selection.
  // Even if they are in another node!
  // XXX Although the comment mentioned that this may scan other text nodes,
  //     GetWhiteSpaceEndPoint() scans only in given container node.
  // XXX Looks like that we should make GetWhiteSpaceEndPoint() be a
  //     instance method and stop scanning white-spaces when it reaches
  //     active editing host.
  EditorDOMPoint startPoint = HTMLEditor::GetWhiteSpaceEndPoint(
      startRef, HTMLEditor::ScanDirection::Backward);
  if (!IsDescendantOfEditorRoot(
          EditorBase::GetNodeAtRangeOffsetPoint(startPoint))) {
    return nullptr;
  }
  EditorDOMPoint endPoint = HTMLEditor::GetWhiteSpaceEndPoint(
      endRef, HTMLEditor::ScanDirection::Forward);
  EditorRawDOMPoint lastRawPoint(endPoint);
  lastRawPoint.RewindOffset();
  if (!IsDescendantOfEditorRoot(
          EditorBase::GetNodeAtRangeOffsetPoint(lastRawPoint))) {
    return nullptr;
  }

  RefPtr<nsRange> range =
      nsRange::Create(startPoint.ToRawRangeBoundary(),
                      endPoint.ToRawRangeBoundary(), IgnoreErrors());
  NS_WARNING_ASSERTION(range, "nsRange::Create() failed");
  return range.forget();
}

already_AddRefed<nsRange> HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const AbstractRange& aAbstractRange, EditSubAction aEditSubAction) const {
  if (!aAbstractRange.IsPositioned()) {
    return nullptr;
  }
  return CreateRangeExtendedToHardLineStartAndEnd(
      aAbstractRange.StartRef(), aAbstractRange.EndRef(), aEditSubAction);
}

template <typename SPT, typename SRT, typename EPT, typename ERT>
already_AddRefed<nsRange> HTMLEditor::CreateRangeExtendedToHardLineStartAndEnd(
    const RangeBoundaryBase<SPT, SRT>& aStartRef,
    const RangeBoundaryBase<EPT, ERT>& aEndRef,
    EditSubAction aEditSubAction) const {
  if (NS_WARN_IF(!aStartRef.IsSet()) || NS_WARN_IF(!aEndRef.IsSet())) {
    return nullptr;
  }

  RangeBoundaryBase<SPT, SRT> startRef(aStartRef);
  RangeBoundaryBase<EPT, ERT> endRef(aEndRef);
  SelectBRElementIfCollapsedInEmptyBlock(startRef, endRef);

  // Make a new adjusted range to represent the appropriate block content.
  // This is tricky.  The basic idea is to push out the range endpoints to
  // truly enclose the blocks that we will affect.

  // Make sure that the new range ends up to be in the editable section.
  // XXX Looks like that this check wastes the time.  Perhaps, we should
  //     implement a method which checks both two DOM points in the editor
  //     root.

  EditorDOMPoint startPoint =
      GetCurrentHardLineStartPoint(startRef, aEditSubAction);
  // XXX GetCurrentHardLineStartPoint() may return point of editing
  //     host.  Perhaps, we should change it and stop checking it here
  //     since this check may be expensive.
  if (!IsDescendantOfEditorRoot(
          EditorBase::GetNodeAtRangeOffsetPoint(startPoint))) {
    return nullptr;
  }
  EditorDOMPoint endPoint = GetCurrentHardLineEndPoint(endRef);
  EditorRawDOMPoint lastRawPoint(endPoint);
  lastRawPoint.RewindOffset();
  // XXX GetCurrentHardLineEndPoint() may return point of editing host.
  //     Perhaps, we should change it and stop checking it here since this
  //     check may be expensive.
  if (!IsDescendantOfEditorRoot(
          EditorBase::GetNodeAtRangeOffsetPoint(lastRawPoint))) {
    return nullptr;
  }

  RefPtr<nsRange> range =
      nsRange::Create(startPoint.ToRawRangeBoundary(),
                      endPoint.ToRawRangeBoundary(), IgnoreErrors());
  NS_WARNING_ASSERTION(range, "nsRange::Create() failed");
  return range.forget();
}

nsresult HTMLEditor::SplitInlinesAndCollectEditTargetNodes(
    nsTArray<RefPtr<nsRange>>& aArrayOfRanges,
    nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
    EditSubAction aEditSubAction,
    CollectNonEditableNodes aCollectNonEditableNodes) {
  nsresult rv = SplitTextNodesAtRangeEnd(aArrayOfRanges);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::SplitTextNodesAtRangeEnd() failed");
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = SplitParentInlineElementsAtRangeEdges(aArrayOfRanges);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::SplitParentInlineElementsAtRangeEdges() failed");
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = CollectEditTargetNodes(aArrayOfRanges, aOutArrayOfContents,
                              aEditSubAction, aCollectNonEditableNodes);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollectEditTargetNodes() failed");
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = MaybeSplitElementsAtEveryBRElement(aOutArrayOfContents, aEditSubAction);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::MaybeSplitElementsAtEveryBRElement() failed");
  return rv;
}

nsresult HTMLEditor::SplitTextNodesAtRangeEnd(
    nsTArray<RefPtr<nsRange>>& aArrayOfRanges) {
  // Split text nodes. This is necessary, since given ranges may end in text
  // nodes in case where part of a pre-formatted elements needs to be moved.
  ErrorResult error;
  IgnoredErrorResult ignoredError;
  for (RefPtr<nsRange>& range : aArrayOfRanges) {
    EditorDOMPoint atEnd(range->EndRef());
    if (NS_WARN_IF(!atEnd.IsSet()) || !atEnd.IsInTextNode()) {
      continue;
    }

    if (!atEnd.IsStartOfContainer() && !atEnd.IsEndOfContainer()) {
      // Split the text node.
      nsCOMPtr<nsIContent> newLeftNode = SplitNodeWithTransaction(atEnd, error);
      if (NS_WARN_IF(Destroyed())) {
        error = NS_ERROR_EDITOR_DESTROYED;
      }
      if (error.Failed()) {
        NS_WARNING_ASSERTION(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED),
                             "HTMLEditor::SplitNodeWithTransaction() failed");
        return error.StealNSResult();
      }

      // Correct the range.
      // The new end parent becomes the parent node of the text.
      EditorRawDOMPoint atContainerOfSplitNode(atEnd.GetContainer());
      MOZ_ASSERT(!range->IsInSelection());
      range->SetEnd(atContainerOfSplitNode, ignoredError);
      NS_WARNING_ASSERTION(!ignoredError.Failed(),
                           "nsRange::SetEnd() failed, but ignored");
      ignoredError.SuppressException();
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::SplitParentInlineElementsAtRangeEdges(
    nsTArray<RefPtr<nsRange>>& aArrayOfRanges) {
  nsTArray<OwningNonNull<RangeItem>> rangeItemArray;
  rangeItemArray.AppendElements(aArrayOfRanges.Length());

  // First register ranges for special editor gravity
  for (auto& rangeItem : rangeItemArray) {
    rangeItem = new RangeItem();
    rangeItem->StoreRange(*aArrayOfRanges[0]);
    RangeUpdaterRef().RegisterRangeItem(*rangeItem);
    aArrayOfRanges.RemoveElementAt(0);
  }
  // Now bust up inlines.
  nsresult rv = NS_OK;
  for (auto& item : Reversed(rangeItemArray)) {
    // MOZ_KnownLive because 'rangeItemArray' is guaranteed to keep it alive.
    rv = SplitParentInlineElementsAtRangeEdges(MOZ_KnownLive(*item));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::SplitParentInlineElementsAtRangeEdges() failed");
      break;
    }
  }
  // Then unregister the ranges
  for (auto& item : rangeItemArray) {
    RangeUpdaterRef().DropRangeItem(item);
    RefPtr<nsRange> range = item->GetRange();
    if (range) {
      aArrayOfRanges.AppendElement(range);
    }
  }

  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  return NS_OK;
}

nsresult HTMLEditor::CollectEditTargetNodes(
    nsTArray<RefPtr<nsRange>>& aArrayOfRanges,
    nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
    EditSubAction aEditSubAction,
    CollectNonEditableNodes aCollectNonEditableNodes) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // Gather up a list of all the nodes
  for (auto& range : aArrayOfRanges) {
    DOMSubtreeIterator iter;
    nsresult rv = iter.Init(*range);
    if (NS_FAILED(rv)) {
      NS_WARNING("DOMSubtreeIterator::Init() failed");
      return rv;
    }
    if (aOutArrayOfContents.IsEmpty()) {
      iter.AppendAllNodesToArray(aOutArrayOfContents);
    } else {
      AutoTArray<OwningNonNull<nsIContent>, 24> arrayOfTopChildren;
      iter.AppendNodesToArray(
          +[](nsINode& aNode, void* aArray) -> bool {
            MOZ_ASSERT(aArray);
            return !static_cast<nsTArray<OwningNonNull<nsIContent>>*>(aArray)
                        ->Contains(&aNode);
          },
          arrayOfTopChildren, &aOutArrayOfContents);
      aOutArrayOfContents.AppendElements(std::move(arrayOfTopChildren));
    }
    if (aCollectNonEditableNodes == CollectNonEditableNodes::No) {
      for (size_t i = aOutArrayOfContents.Length(); i > 0; --i) {
        if (!EditorUtils::IsEditableContent(aOutArrayOfContents[i - 1],
                                            EditorType::HTML)) {
          aOutArrayOfContents.RemoveElementAt(i - 1);
        }
      }
    }
  }

  switch (aEditSubAction) {
    case EditSubAction::eCreateOrRemoveBlock:
      // Certain operations should not act on li's and td's, but rather inside
      // them.  Alter the list as needed.
      for (int32_t i = aOutArrayOfContents.Length() - 1; i >= 0; i--) {
        OwningNonNull<nsIContent> content = aOutArrayOfContents[i];
        if (HTMLEditUtils::IsListItem(content)) {
          aOutArrayOfContents.RemoveElementAt(i);
          CollectChildren(*content, aOutArrayOfContents, i,
                          CollectListChildren::Yes, CollectTableChildren::Yes,
                          aCollectNonEditableNodes);
        }
      }
      // Empty text node shouldn't be selected if unnecessary
      for (int32_t i = aOutArrayOfContents.Length() - 1; i >= 0; i--) {
        if (Text* text = aOutArrayOfContents[i]->GetAsText()) {
          // Don't select empty text except to empty block
          if (!IsVisibleTextNode(*text)) {
            aOutArrayOfContents.RemoveElementAt(i);
          }
        }
      }
      break;
    case EditSubAction::eCreateOrChangeList: {
      for (size_t i = aOutArrayOfContents.Length(); i > 0; i--) {
        // Scan for table elements.  If we find table elements other than
        // table, replace it with a list of any editable non-table content
        // because if a selection range starts from end in a table-cell and
        // ends at or starts from outside the `<table>`, we need to make
        // lists in each selected table-cells.
        OwningNonNull<nsIContent> content = aOutArrayOfContents[i - 1];
        if (HTMLEditUtils::IsAnyTableElementButNotTable(content)) {
          // XXX aCollectNonEditableNodes is ignored here.  Maybe a bug.
          aOutArrayOfContents.RemoveElementAt(i - 1);
          CollectChildren(content, aOutArrayOfContents, i - 1,
                          CollectListChildren::No, CollectTableChildren::Yes,
                          CollectNonEditableNodes::Yes);
        }
      }
      // If there is only one node in the array, and it is a `<div>`,
      // `<blockquote>` or a list element, then look inside of it until we
      // find inner list or content.
      if (aOutArrayOfContents.Length() != 1) {
        break;
      }
      Element* deepestDivBlockquoteOrListElement =
          GetDeepestEditableOnlyChildDivBlockquoteOrListElement(
              aOutArrayOfContents[0]);
      if (!deepestDivBlockquoteOrListElement) {
        break;
      }
      if (deepestDivBlockquoteOrListElement->IsAnyOfHTMLElements(
              nsGkAtoms::div, nsGkAtoms::blockquote)) {
        aOutArrayOfContents.Clear();
        // XXX Before we're called, non-editable nodes are ignored.  However,
        //     we may append non-editable nodes here.
        CollectChildren(*deepestDivBlockquoteOrListElement, aOutArrayOfContents,
                        0, CollectListChildren::No, CollectTableChildren::No,
                        CollectNonEditableNodes::Yes);
        break;
      }
      aOutArrayOfContents.ReplaceElementAt(
          0, OwningNonNull<nsIContent>(*deepestDivBlockquoteOrListElement));
      break;
    }
    case EditSubAction::eOutdent:
    case EditSubAction::eIndent:
    case EditSubAction::eSetPositionToAbsolute:
      // Indent/outdent already do something special for list items, but we
      // still need to make sure we don't act on table elements
      for (int32_t i = aOutArrayOfContents.Length() - 1; i >= 0; i--) {
        OwningNonNull<nsIContent> content = aOutArrayOfContents[i];
        if (HTMLEditUtils::IsAnyTableElementButNotTable(content)) {
          aOutArrayOfContents.RemoveElementAt(i);
          CollectChildren(*content, aOutArrayOfContents, i,
                          CollectListChildren::Yes, CollectTableChildren::Yes,
                          aCollectNonEditableNodes);
        }
      }
      break;
    default:
      break;
  }

  // Outdent should look inside of divs.
  if (aEditSubAction == EditSubAction::eOutdent && !IsCSSEnabled()) {
    for (int32_t i = aOutArrayOfContents.Length() - 1; i >= 0; i--) {
      OwningNonNull<nsIContent> content = aOutArrayOfContents[i];
      if (content->IsHTMLElement(nsGkAtoms::div)) {
        aOutArrayOfContents.RemoveElementAt(i);
        CollectChildren(*content, aOutArrayOfContents, i,
                        CollectListChildren::No, CollectTableChildren::No,
                        aCollectNonEditableNodes);
      }
    }
  }

  return NS_OK;
}

nsresult HTMLEditor::MaybeSplitElementsAtEveryBRElement(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    EditSubAction aEditSubAction) {
  // Post-process the list to break up inline containers that contain br's, but
  // only for operations that might care, like making lists or paragraphs
  switch (aEditSubAction) {
    case EditSubAction::eCreateOrRemoveBlock:
    case EditSubAction::eMergeBlockContents:
    case EditSubAction::eCreateOrChangeList:
    case EditSubAction::eSetOrClearAlignment:
    case EditSubAction::eSetPositionToAbsolute:
    case EditSubAction::eIndent:
    case EditSubAction::eOutdent:
      for (int32_t i = aArrayOfContents.Length() - 1; i >= 0; i--) {
        OwningNonNull<nsIContent>& content = aArrayOfContents[i];
        if (HTMLEditUtils::IsInlineElement(content) &&
            HTMLEditUtils::IsContainerNode(content) && !content->IsText()) {
          AutoTArray<OwningNonNull<nsIContent>, 24> arrayOfInlineContents;
          // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
          // alive.
          nsresult rv = SplitElementsAtEveryBRElement(MOZ_KnownLive(content),
                                                      arrayOfInlineContents);
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::SplitElementsAtEveryBRElement() failed");
            return rv;
          }

          // Put these nodes in aArrayOfContents, replacing the current node
          aArrayOfContents.RemoveElementAt(i);
          aArrayOfContents.InsertElementsAt(i, arrayOfInlineContents);
        }
      }
      return NS_OK;
    default:
      return NS_OK;
  }
}

Element* HTMLEditor::GetParentListElementAtSelection() const {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  for (uint32_t i = 0; i < SelectionRefPtr()->RangeCount(); ++i) {
    nsRange* range = SelectionRefPtr()->GetRangeAt(i);
    for (nsINode* parent = range->GetClosestCommonInclusiveAncestor(); parent;
         parent = parent->GetParentNode()) {
      if (HTMLEditUtils::IsAnyListElement(parent)) {
        return parent->AsElement();
      }
    }
  }
  return nullptr;
}

Element* HTMLEditor::GetDeepestEditableOnlyChildDivBlockquoteOrListElement(
    nsINode& aNode) {
  if (!aNode.IsElement()) {
    return nullptr;
  }
  // XXX If aNode is non-editable, shouldn't we return nullptr here?
  Element* parentElement = nullptr;
  for (nsIContent* content = aNode.AsContent();
       content && content->IsElement() &&
       (content->IsAnyOfHTMLElements(nsGkAtoms::div, nsGkAtoms::blockquote) ||
        HTMLEditUtils::IsAnyListElement(content));
       content = content->GetFirstChild()) {
    if (CountEditableChildren(content) != 1) {
      return content->AsElement();
    }
    parentElement = content->AsElement();
  }
  return parentElement;
}

nsresult HTMLEditor::SplitParentInlineElementsAtRangeEdges(
    RangeItem& aRangeItem) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!aRangeItem.IsCollapsed()) {
    nsCOMPtr<nsIContent> mostAncestorInlineContentAtEnd =
        GetMostAncestorInlineElement(*aRangeItem.mEndContainer);

    if (mostAncestorInlineContentAtEnd) {
      SplitNodeResult splitEndInlineResult = SplitNodeDeepWithTransaction(
          *mostAncestorInlineContentAtEnd, aRangeItem.EndPoint(),
          SplitAtEdges::eDoNotCreateEmptyContainer);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (splitEndInlineResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) failed");
        return splitEndInlineResult.Rv();
      }
      EditorRawDOMPoint splitPointAtEnd(splitEndInlineResult.SplitPoint());
      if (!splitPointAtEnd.IsSet()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) didn't return split point");
        return NS_ERROR_FAILURE;
      }
      aRangeItem.mEndContainer = splitPointAtEnd.GetContainer();
      aRangeItem.mEndOffset = splitPointAtEnd.Offset();
    }
  }

  nsCOMPtr<nsIContent> mostAncestorInlineContentAtStart =
      GetMostAncestorInlineElement(*aRangeItem.mStartContainer);

  if (mostAncestorInlineContentAtStart) {
    SplitNodeResult splitStartInlineResult = SplitNodeDeepWithTransaction(
        *mostAncestorInlineContentAtStart, aRangeItem.StartPoint(),
        SplitAtEdges::eDoNotCreateEmptyContainer);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (splitStartInlineResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
          "eDoNotCreateEmptyContainer) failed");
      return splitStartInlineResult.Rv();
    }
    // XXX If we split only here because of collapsed range, we're modifying
    //     only start point of aRangeItem.  Shouldn't we modify end point here
    //     if it's collapsed?
    EditorRawDOMPoint splitPointAtStart(splitStartInlineResult.SplitPoint());
    if (!splitPointAtStart.IsSet()) {
      NS_WARNING(
          "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
          "eDoNotCreateEmptyContainer) didn't return split point");
      return NS_ERROR_FAILURE;
    }
    aRangeItem.mStartContainer = splitPointAtStart.GetContainer();
    aRangeItem.mStartOffset = splitPointAtStart.Offset();
  }

  return NS_OK;
}

nsresult HTMLEditor::SplitElementsAtEveryBRElement(
    nsIContent& aMostAncestorToBeSplit,
    nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // First build up a list of all the break nodes inside the inline container.
  AutoTArray<OwningNonNull<HTMLBRElement>, 24> arrayOfBRElements;
  DOMIterator iter(aMostAncestorToBeSplit);
  iter.AppendAllNodesToArray(arrayOfBRElements);

  // If there aren't any breaks, just put inNode itself in the array
  if (arrayOfBRElements.IsEmpty()) {
    aOutArrayOfContents.AppendElement(aMostAncestorToBeSplit);
    return NS_OK;
  }

  // Else we need to bust up aMostAncestorToBeSplit along all the breaks
  nsCOMPtr<nsIContent> nextContent = &aMostAncestorToBeSplit;
  for (OwningNonNull<HTMLBRElement>& brElement : arrayOfBRElements) {
    EditorDOMPoint atBRNode(brElement);
    if (NS_WARN_IF(!atBRNode.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
        *nextContent, atBRNode, SplitAtEdges::eAllowToCreateEmptyContainer);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (splitNodeResult.Failed()) {
      NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
      return splitNodeResult.Rv();
    }

    // Put previous node at the split point.
    if (splitNodeResult.GetPreviousNode()) {
      // Might not be a left node.  A break might have been at the very
      // beginning of inline container, in which case
      // SplitNodeDeepWithTransaction() would not actually split anything.
      aOutArrayOfContents.AppendElement(*splitNodeResult.GetPreviousNode());
    }

    // Move break outside of container and also put in node list
    EditorDOMPoint atNextNode(splitNodeResult.GetNextNode());
    // MOZ_KnownLive because 'arrayOfBRElements' is guaranteed to keep it alive.
    nsresult rv = MoveNodeWithTransaction(MOZ_KnownLive(brElement), atNextNode);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return rv;
    }
    aOutArrayOfContents.AppendElement(brElement);

    nextContent = splitNodeResult.GetNextNode();
  }

  // Now tack on remaining next node.
  aOutArrayOfContents.AppendElement(*nextContent);

  return NS_OK;
}

nsIContent* HTMLEditor::GetMostAncestorInlineElement(nsINode& aNode) const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!aNode.IsContent() || HTMLEditUtils::IsBlockElement(*aNode.AsContent())) {
    return nullptr;
  }

  Element* host = GetActiveEditingHost();
  if (NS_WARN_IF(!host)) {
    return nullptr;
  }

  // If aNode is the editing host itself, there is no modifiable inline
  // parent.
  if (&aNode == host) {
    return nullptr;
  }

  // If aNode is outside of the <body> element, we don't support to edit
  // such elements for now.
  // XXX This should be MOZ_ASSERT after fixing bug 1413131 for avoiding
  //     calling this expensive method.
  if (NS_WARN_IF(!EditorUtils::IsDescendantOf(aNode, *host))) {
    return nullptr;
  }

  if (!aNode.GetParent()) {
    return aNode.AsContent();
  }

  // Looks for the highest inline parent in the editing host.
  nsIContent* topMostInlineContent = aNode.AsContent();
  for (nsIContent* content : aNode.AncestorsOfType<nsIContent>()) {
    if (content == host || !HTMLEditUtils::IsInlineElement(*content)) {
      break;
    }
    topMostInlineContent = content;
  }
  return topMostInlineContent;
}

// static
void HTMLEditor::MakeTransitionList(
    const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    nsTArray<bool>& aTransitionArray) {
  nsINode* prevParent = nullptr;
  aTransitionArray.EnsureLengthAtLeast(aArrayOfContents.Length());
  for (uint32_t i = 0; i < aArrayOfContents.Length(); i++) {
    aTransitionArray[i] = aArrayOfContents[i]->GetParentNode() != prevParent;
    prevParent = aArrayOfContents[i]->GetParentNode();
  }
}

Element* HTMLEditor::GetNearestAncestorListItemElement(
    nsIContent& aContent) const {
  // XXX Why don't we test whether aContent is in an editing host?
  if (HTMLEditUtils::IsListItem(&aContent)) {
    return aContent.AsElement();
  }

  // XXX This loop is too expensive since calling IsDescendantOfEditorRoot()
  //     a lot.  Rewrite this with caching active editing host and check
  //     whether we reach it or not.
  for (Element* parentElement = aContent.GetParentElement();
       parentElement && IsDescendantOfEditorRoot(parentElement) &&
       !HTMLEditUtils::IsAnyTableElement(parentElement);
       parentElement = parentElement->GetParentElement()) {
    if (HTMLEditUtils::IsListItem(parentElement)) {
      return parentElement;
    }
  }
  return nullptr;
}

nsresult HTMLEditor::HandleInsertParagraphInHeadingElement(Element& aHeader,
                                                           nsINode& aNode,
                                                           int32_t aOffset) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // Remember where the header is
  nsCOMPtr<nsINode> headerParent = aHeader.GetParentNode();
  int32_t offset = headerParent ? headerParent->ComputeIndexOf(&aHeader) : -1;

  // Get ws code to adjust any ws
  nsCOMPtr<nsINode> node = &aNode;
  nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks(
      *this, address_of(node), &aOffset);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() failed");
    return rv;
  }
  if (!node->IsContent()) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() returned "
        "Document or something non-content node");
    return NS_ERROR_FAILURE;
  }

  // Split the header
  SplitNodeResult splitHeaderResult =
      SplitNodeDeepWithTransaction(aHeader, EditorDOMPoint(node, aOffset),
                                   SplitAtEdges::eAllowToCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      splitHeaderResult.Succeeded(),
      "HTMLEditor::SplitNodeDeepWithTransaction() failed, but ignored");

  // If the previous heading of split point is empty, put a padding <br>
  // element for empty last line into it.
  nsCOMPtr<nsIContent> prevItem = GetPriorHTMLSibling(&aHeader);
  if (prevItem) {
    MOZ_DIAGNOSTIC_ASSERT(HTMLEditUtils::IsHeader(*prevItem));
    if (IsEmptyNode(*prevItem)) {
      CreateElementResult createPaddingBRResult =
          InsertPaddingBRElementForEmptyLastLineWithTransaction(
              EditorDOMPoint(prevItem, 0));
      if (createPaddingBRResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction("
            ") failed");
        return createPaddingBRResult.Rv();
      }
    }
  }

  // If the new (righthand) header node is empty, delete it
  if (IsEmptyBlockElement(aHeader, IgnoreSingleBR::Yes)) {
    nsresult rv = DeleteNodeWithTransaction(aHeader);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }
    // Layout tells the caret to blink in a weird place if we don't place a
    // break after the header.
    nsCOMPtr<nsIContent> sibling;
    if (aHeader.GetNextSibling()) {
      sibling = GetNextHTMLSibling(aHeader.GetNextSibling());
    }
    if (!sibling || !sibling->IsHTMLElement(nsGkAtoms::br)) {
      TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
      mTypeInState->ClearAllProps();

      // Create a paragraph
      nsStaticAtom& paraAtom = DefaultParagraphSeparatorTagName();
      // We want a wrapper element even if we separate with <br>
      EditorDOMPoint nextToHeader(headerParent, offset + 1);
      RefPtr<Element> pOrDivElement = CreateNodeWithTransaction(
          &paraAtom == nsGkAtoms::br ? *nsGkAtoms::p : MOZ_KnownLive(paraAtom),
          nextToHeader);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!pOrDivElement) {
        NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }

      // Append a <br> to it
      RefPtr<Element> brElement =
          InsertBRElementWithTransaction(EditorDOMPoint(pOrDivElement, 0));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!brElement) {
        NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }

      // Set selection to before the break
      nsresult rv = CollapseSelectionToStartOf(*pOrDivElement);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::CollapseSelectionToStartOf() failed");
      return rv;
    }

    EditorRawDOMPoint afterSibling(EditorRawDOMPoint::After(*sibling));
    if (NS_WARN_IF(!afterSibling.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    // Put selection after break
    rv = CollapseSelectionTo(afterSibling);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionTo() failed");
    return rv;
  }

  // Put selection at front of righthand heading
  rv = CollapseSelectionToStartOf(aHeader);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionToStartOf() failed");
  return rv;
}

EditActionResult HTMLEditor::HandleInsertParagraphInParagraph(
    Element& aParentDivOrP) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  EditorDOMPoint atStartOfSelection(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return EditActionResult(NS_ERROR_FAILURE);
  }
  MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

  // We shouldn't create new anchor element which has non-empty href unless
  // splitting middle of it because we assume that users don't want to create
  // *same* anchor element across two or more paragraphs in most cases.
  // So, adjust selection start if it's edge of anchor element(s).
  // XXX We don't support white-space collapsing in these cases since it needs
  //     some additional work with WhiteSpaceVisibilityKeeper but it's not usual
  //     case. E.g., |<a href="foo"><b>foo []</b> </a>|
  if (atStartOfSelection.IsStartOfContainer()) {
    for (nsIContent* container = atStartOfSelection.GetContainerAsContent();
         container && container != &aParentDivOrP;
         container = container->GetParent()) {
      if (HTMLEditUtils::IsLink(container)) {
        // Found link should be only in right node.  So, we shouldn't split
        // it.
        atStartOfSelection.Set(container);
        // Even if we found an anchor element, don't break because DOM API
        // allows to nest anchor elements.
      }
      // If the container is middle of its parent, stop adjusting split point.
      if (container->GetPreviousSibling()) {
        // XXX Should we check if previous sibling is visible content?
        //     E.g., should we ignore comment node, invisible <br> element?
        break;
      }
    }
  }
  // We also need to check if selection is at invisible <br> element at end
  // of an <a href="foo"> element because editor inserts a <br> element when
  // user types Enter key after a white-space which is at middle of
  // <a href="foo"> element and when setting selection at end of the element,
  // selection becomes referring the <br> element.  We may need to change this
  // behavior later if it'd be standardized.
  else if (atStartOfSelection.IsEndOfContainer() ||
           atStartOfSelection.IsBRElementAtEndOfContainer()) {
    // If there are 2 <br> elements, the first <br> element is visible.  E.g.,
    // |<a href="foo"><b>boo[]<br></b><br></a>|, we should split the <a>
    // element.  Otherwise, E.g., |<a href="foo"><b>boo[]<br></b></a>|,
    // we should not split the <a> element and ignore inline elements in it.
    bool foundBRElement = atStartOfSelection.IsBRElementAtEndOfContainer();
    for (nsIContent* container = atStartOfSelection.GetContainerAsContent();
         container && container != &aParentDivOrP;
         container = container->GetParent()) {
      if (HTMLEditUtils::IsLink(container)) {
        // Found link should be only in left node.  So, we shouldn't split it.
        atStartOfSelection.SetAfter(container);
        // Even if we found an anchor element, don't break because DOM API
        // allows to nest anchor elements.
      }
      // If the container is middle of its parent, stop adjusting split point.
      if (nsIContent* nextSibling = container->GetNextSibling()) {
        if (foundBRElement) {
          // If we've already found a <br> element, we assume found node is
          // visible <br> or something other node.
          // XXX Should we check if non-text data node like comment?
          break;
        }

        // XXX Should we check if non-text data node like comment?
        if (!nextSibling->IsHTMLElement(nsGkAtoms::br)) {
          break;
        }
        foundBRElement = true;
      }
    }
  }

  bool doesCRCreateNewP = GetReturnInParagraphCreatesNewParagraph();
  bool splitAfterNewBR = false;
  nsCOMPtr<nsIContent> brContent;

  EditorDOMPoint pointToSplitParentDivOrP(atStartOfSelection);

  EditorDOMPoint pointToInsertBR;
  if (doesCRCreateNewP && atStartOfSelection.GetContainer() == &aParentDivOrP) {
    // We are at the edges of the block, so, we don't need to create new <br>.
    brContent = nullptr;
  } else if (atStartOfSelection.IsInTextNode()) {
    // at beginning of text node?
    if (atStartOfSelection.IsStartOfContainer()) {
      // is there a BR prior to it?
      brContent = GetPriorHTMLSibling(atStartOfSelection.GetContainer());
      if (!brContent || !IsVisibleBRElement(brContent) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*brContent)) {
        pointToInsertBR.Set(atStartOfSelection.GetContainer());
        brContent = nullptr;
      }
    } else if (atStartOfSelection.IsEndOfContainer()) {
      // we're at the end of text node...
      // is there a BR after to it?
      brContent = GetNextHTMLSibling(atStartOfSelection.GetContainer());
      if (!brContent || !IsVisibleBRElement(brContent) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*brContent)) {
        pointToInsertBR.SetAfter(atStartOfSelection.GetContainer());
        NS_WARNING_ASSERTION(
            pointToInsertBR.IsSet(),
            "Failed to set to after the container  of selection start");
        brContent = nullptr;
      }
    } else {
      if (doesCRCreateNewP) {
        ErrorResult error;
        nsCOMPtr<nsIContent> newLeftDivOrP =
            SplitNodeWithTransaction(pointToSplitParentDivOrP, error);
        if (NS_WARN_IF(Destroyed())) {
          error = NS_ERROR_EDITOR_DESTROYED;
        }
        if (error.Failed()) {
          NS_WARNING_ASSERTION(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED),
                               "HTMLEditor::SplitNodeWithTransaction() failed");
          return EditActionResult(error.StealNSResult());
        }
        pointToSplitParentDivOrP.SetToEndOf(newLeftDivOrP);
      }

      // We need to put new <br> after the left node if given node was split
      // above.
      pointToInsertBR.Set(pointToSplitParentDivOrP.GetContainer());
      DebugOnly<bool> advanced = pointToInsertBR.AdvanceOffset();
      NS_WARNING_ASSERTION(advanced,
                           "Failed to advance offset to after the container "
                           "of selection start");
    }
  } else {
    // not in a text node.
    // is there a BR prior to it?
    nsCOMPtr<nsIContent> nearContent =
        GetPreviousEditableHTMLNode(atStartOfSelection);
    if (!nearContent || !IsVisibleBRElement(nearContent) ||
        EditorUtils::IsPaddingBRElementForEmptyLastLine(*nearContent)) {
      // is there a BR after it?
      nearContent = GetNextEditableHTMLNode(atStartOfSelection);
      if (!nearContent || !IsVisibleBRElement(nearContent) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*nearContent)) {
        pointToInsertBR = atStartOfSelection;
        splitAfterNewBR = true;
      }
    }
    if (!pointToInsertBR.IsSet() && nearContent->IsHTMLElement(nsGkAtoms::br)) {
      brContent = nearContent;
    }
  }
  if (pointToInsertBR.IsSet()) {
    // if CR does not create a new P, default to BR creation
    if (NS_WARN_IF(!doesCRCreateNewP)) {
      return EditActionResult(NS_OK);
    }

    brContent = InsertBRElementWithTransaction(pointToInsertBR);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        brContent,
        "HTMLEditor::InsertBRElementWithTransaction() failed, but ignored");
    if (splitAfterNewBR) {
      // We split the parent after the br we've just inserted.
      pointToSplitParentDivOrP.SetAfter(brContent);
      NS_WARNING_ASSERTION(pointToSplitParentDivOrP.IsSet(),
                           "Failed to set after the new <br>");
    }
  }
  EditActionResult result(
      SplitParagraph(aParentDivOrP, pointToSplitParentDivOrP, brContent));
  NS_WARNING_ASSERTION(result.Succeeded(),
                       "HTMLEditor::SplitParagraph() failed");
  result.MarkAsHandled();
  return result;
}

template <typename PT, typename CT>
nsresult HTMLEditor::SplitParagraph(
    Element& aParentDivOrP, const EditorDOMPointBase<PT, CT>& aStartOfRightNode,
    nsIContent* aNextBRNode) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsCOMPtr<nsINode> selNode = aStartOfRightNode.GetContainer();
  int32_t selOffset = aStartOfRightNode.Offset();
  nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks(
      *this, address_of(selNode), &selOffset);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() failed");
    return rv;
  }
  if (!selNode->IsContent()) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() returned "
        "Document or something non-content node");
    return NS_ERROR_FAILURE;
  }

  // Split the paragraph.
  SplitNodeResult splitDivOrPResult = SplitNodeDeepWithTransaction(
      aParentDivOrP, EditorDOMPoint(selNode, selOffset),
      SplitAtEdges::eAllowToCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (splitDivOrPResult.Failed()) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
    return splitDivOrPResult.Rv();
  }
  if (!splitDivOrPResult.DidSplit()) {
    NS_WARNING(
        "HTMLEditor::SplitNodeDeepWithTransaction() didn't split any nodes");
    return NS_ERROR_FAILURE;
  }

  // Get rid of the break, if it is visible (otherwise it may be needed to
  // prevent an empty p).
  if (aNextBRNode && IsVisibleBRElement(aNextBRNode)) {
    rv = DeleteNodeWithTransaction(*aNextBRNode);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  // Remove ID attribute on the paragraph from the existing right node.
  rv = RemoveAttributeWithTransaction(aParentDivOrP, *nsGkAtoms::id);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::id) failed");
    return rv;
  }

  // We need to ensure to both paragraphs visible even if they are empty.
  // However, padding <br> element for empty last line isn't useful in this
  // case because it'll be ignored by PlaintextSerializer.  Additionally,
  // it'll be exposed as <br> with Element.innerHTML.  Therefore, we can use
  // normal <br> elements for placeholder in this case.  Note that Chromium
  // also behaves so.
  if (splitDivOrPResult.GetPreviousNode()->IsElement()) {
    rv = InsertBRElementIfEmptyBlockElement(
        MOZ_KnownLive(*splitDivOrPResult.GetPreviousNode()->AsElement()));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::InsertBRElementIfEmptyBlockElement() failed");
      return rv;
    }
  }
  if (splitDivOrPResult.GetNextNode()->IsElement()) {
    rv = InsertBRElementIfEmptyBlockElement(
        MOZ_KnownLive(*splitDivOrPResult.GetNextNode()->AsElement()));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::InsertBRElementIfEmptyBlockElement() failed");
      return rv;
    }
  }

  // selection to beginning of right hand para;
  // look inside any containers that are up front.
  nsCOMPtr<nsIContent> child = HTMLEditUtils::GetFirstLeafChild(
      aParentDivOrP, ChildBlockBoundary::TreatAsLeaf);
  if (child && (child->IsText() || HTMLEditUtils::IsContainerNode(*child))) {
    nsresult rv = CollapseSelectionToStartOf(*child);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionToStartOf() failed, but ignored");
  } else {
    nsresult rv = CollapseSelectionTo(EditorRawDOMPoint(child));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionTo() failed, but ignored");
  }
  return NS_OK;
}

nsresult HTMLEditor::HandleInsertParagraphInListItemElement(Element& aListItem,
                                                            nsINode& aNode,
                                                            int32_t aOffset) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsListItem(&aListItem));

  // Get the item parent and the active editing host.
  RefPtr<Element> host = GetActiveEditingHost();

  // If we are in an empty item, then we want to pop up out of the list, but
  // only if prefs say it's okay and if the parent isn't the active editing
  // host.
  if (host != aListItem.GetParentElement() &&
      IsEmptyBlockElement(aListItem, IgnoreSingleBR::Yes)) {
    nsCOMPtr<nsIContent> leftListNode = aListItem.GetParent();
    // Are we the last list item in the list?
    if (!IsLastEditableChild(&aListItem)) {
      // We need to split the list!
      EditorDOMPoint atListItem(&aListItem);
      ErrorResult error;
      leftListNode = SplitNodeWithTransaction(atListItem, error);
      if (NS_WARN_IF(Destroyed())) {
        error = NS_ERROR_EDITOR_DESTROYED;
      }
      if (error.Failed()) {
        NS_WARNING_ASSERTION(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED),
                             "HTMLEditor::SplitNodeWithTransaction() failed");
        return error.StealNSResult();
      }
    }

    // Are we in a sublist?
    EditorDOMPoint atNextSiblingOfLeftList(leftListNode);
    DebugOnly<bool> advanced = atNextSiblingOfLeftList.AdvanceOffset();
    NS_WARNING_ASSERTION(advanced,
                         "Failed to advance offset after the right list node");
    if (HTMLEditUtils::IsAnyListElement(
            atNextSiblingOfLeftList.GetContainer())) {
      // If so, move item out of this list and into the grandparent list
      nsresult rv = MoveNodeWithTransaction(aListItem, atNextSiblingOfLeftList);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
        return rv;
      }
      rv = CollapseSelectionToStartOf(aListItem);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::CollapseSelectionToStartOf() failed");
      return rv;
    }

    // Otherwise kill this item
    nsresult rv = DeleteNodeWithTransaction(aListItem);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }

    // Time to insert a paragraph
    nsStaticAtom& paraAtom = DefaultParagraphSeparatorTagName();
    // We want a wrapper even if we separate with <br>
    RefPtr<Element> pOrDivElement = CreateNodeWithTransaction(
        &paraAtom == nsGkAtoms::br ? *nsGkAtoms::p : MOZ_KnownLive(paraAtom),
        atNextSiblingOfLeftList);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!pOrDivElement) {
      NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }

    // Append a <br> to it
    RefPtr<Element> brElement =
        InsertBRElementWithTransaction(EditorDOMPoint(pOrDivElement, 0));
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!brElement) {
      NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
      return NS_ERROR_FAILURE;
    }

    // Set selection to before the break
    rv = CollapseSelectionToStartOf(*pOrDivElement);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    return rv;
  }

  // Else we want a new list item at the same list level.  Get ws code to
  // adjust any ws.
  nsCOMPtr<nsINode> selNode = &aNode;
  nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks(
      *this, address_of(selNode), &aOffset);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() failed");
    return rv;
  }
  if (!selNode->IsContent()) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks() returned "
        "document node or something non-content node");
    return NS_ERROR_FAILURE;
  }

  // Now split the list item.
  SplitNodeResult splitListItemResult =
      SplitNodeDeepWithTransaction(aListItem, EditorDOMPoint(selNode, aOffset),
                                   SplitAtEdges::eAllowToCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      splitListItemResult.Succeeded(),
      "HTMLEditor::SplitNodeDeepWithTransaction() failed, but ignored");

  // Hack: until I can change the damaged doc range code back to being
  // extra-inclusive, I have to manually detect certain list items that may be
  // left empty.
  nsCOMPtr<nsIContent> prevItem = GetPriorHTMLSibling(&aListItem);
  if (prevItem && HTMLEditUtils::IsListItem(prevItem)) {
    if (IsEmptyNode(*prevItem)) {
      CreateElementResult createPaddingBRResult =
          InsertPaddingBRElementForEmptyLastLineWithTransaction(
              EditorDOMPoint(prevItem, 0));
      if (createPaddingBRResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction("
            ") failed");
        return createPaddingBRResult.Rv();
      }
    } else {
      if (IsEmptyNode(aListItem, true)) {
        if (aListItem.IsAnyOfHTMLElements(nsGkAtoms::dd, nsGkAtoms::dt)) {
          nsCOMPtr<nsINode> list = aListItem.GetParentNode();
          int32_t itemOffset = list ? list->ComputeIndexOf(&aListItem) : -1;

          nsStaticAtom* nextDefinitionListItemTagName =
              aListItem.IsHTMLElement(nsGkAtoms::dt) ? nsGkAtoms::dd
                                                     : nsGkAtoms::dt;
          MOZ_DIAGNOSTIC_ASSERT(itemOffset != -1);
          EditorDOMPoint atNextListItem(list, aListItem.GetNextSibling(),
                                        itemOffset + 1);
          RefPtr<Element> newListItem = CreateNodeWithTransaction(
              MOZ_KnownLive(*nextDefinitionListItemTagName), atNextListItem);
          if (NS_WARN_IF(Destroyed())) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          if (!newListItem) {
            NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
            return NS_ERROR_FAILURE;
          }
          nsresult rv = DeleteNodeWithTransaction(aListItem);
          if (NS_WARN_IF(Destroyed())) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          if (NS_FAILED(rv)) {
            NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
            return rv;
          }
          rv = CollapseSelectionToStartOf(*newListItem);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "HTMLEditor::CollapseSelectionToStartOf() failed");
          return rv;
        }

        RefPtr<Element> brElement;
        nsresult rv = CopyLastEditableChildStylesWithTransaction(
            MOZ_KnownLive(*prevItem->AsElement()), aListItem,
            address_of(brElement));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::CopyLastEditableChildStylesWithTransaction() "
              "failed");
          return NS_ERROR_FAILURE;
        }
        if (brElement) {
          EditorRawDOMPoint atBRNode(brElement);
          if (NS_WARN_IF(!atBRNode.IsSetAndValid())) {
            return NS_ERROR_FAILURE;
          }
          nsresult rv = CollapseSelectionTo(atBRNode);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "HTMLEditor::CollapseSelectionTo() failed");
          return rv;
        }
      } else {
        WSScanResult forwardScanFromStartOfListItemResult =
            WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
                *this, EditorRawDOMPoint(&aListItem, 0));
        if (forwardScanFromStartOfListItemResult.ReachedSpecialContent() ||
            forwardScanFromStartOfListItemResult.ReachedBRElement() ||
            forwardScanFromStartOfListItemResult.ReachedHRElement()) {
          EditorRawDOMPoint atFoundElement(
              forwardScanFromStartOfListItemResult.RawPointAtContent());
          if (NS_WARN_IF(!atFoundElement.IsSetAndValid())) {
            return NS_ERROR_FAILURE;
          }
          nsresult rv = CollapseSelectionTo(atFoundElement);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "HTMLEditor::CollapseSelectionTo() failed");
          return rv;
        }

        // XXX This may be not meaningful position if it reached block element
        //     in aListItem.
        nsresult rv = CollapseSelectionTo(
            forwardScanFromStartOfListItemResult.RawPoint());
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "HTMLEditor::CollapseSelectionTo() failed");
        return rv;
      }
    }
  }

  rv = CollapseSelectionToStartOf(aListItem);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionToStartOf() failed");
  return rv;
}

nsresult HTMLEditor::MoveNodesIntoNewBlockquoteElement(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // The idea here is to put the nodes into a minimal number of blockquotes.
  // When the user blockquotes something, they expect one blockquote.  That
  // may not be possible (for instance, if they have two table cells selected,
  // you need two blockquotes inside the cells).
  RefPtr<Element> curBlock;
  nsCOMPtr<nsINode> prevParent;

  for (auto& content : aArrayOfContents) {
    // If the node is a table element or list item, dive inside
    if (HTMLEditUtils::IsAnyTableElementButNotTable(content) ||
        HTMLEditUtils::IsListItem(content)) {
      // Forget any previous block
      curBlock = nullptr;
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      nsresult rv = MoveNodesIntoNewBlockquoteElement(childContents);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodesIntoNewBlockquoteElement() failed");
        return rv;
      }
    }

    // If the node has different parent than previous node, further nodes in a
    // new parent
    if (prevParent) {
      if (prevParent != content->GetParentNode()) {
        // Forget any previous blockquote node we were using
        curBlock = nullptr;
        prevParent = content->GetParentNode();
      }
    } else {
      prevParent = content->GetParentNode();
    }

    // If no curBlock, make one
    if (!curBlock) {
      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::blockquote,
                                                      EditorDOMPoint(content));
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms:"
            ":blockquote) failed");
        return splitNodeResult.Rv();
      }
      // XXX Perhaps, we should insert the new `<blockquote>` element after
      //     moving all nodes into it since the movement does not cause
      //     running mutation event listeners.
      curBlock = CreateNodeWithTransaction(*nsGkAtoms::blockquote,
                                           splitNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!curBlock) {
        NS_WARNING(
            "EditorBase::CreateNodeWithTransaction(nsGkAtoms::blockquote) "
            "failed");
        return NS_ERROR_FAILURE;
      }
      // remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = curBlock;
      // note: doesn't matter if we set mNewBlockElement multiple times.
    }

    // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to/ keep it alive.
    nsresult rv =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curBlock);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::RemoveBlockContainerElements(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // Intent of this routine is to be used for converting to/from headers,
  // paragraphs, pre, and address.  Those blocks that pretty much just contain
  // inline things...
  RefPtr<Element> blockElement;
  nsCOMPtr<nsIContent> firstContent, lastContent;
  for (auto& content : aArrayOfContents) {
    // If curNode is an <address>, <p>, <hn>, or <pre>, remove it.
    if (HTMLEditUtils::IsFormatNode(content)) {
      // Process any partial progress saved
      if (blockElement) {
        SplitRangeOffFromNodeResult removeMiddleContainerResult =
            SplitRangeOffFromBlockAndRemoveMiddleContainer(
                *blockElement, *firstContent, *lastContent);
        if (removeMiddleContainerResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer() "
              "failed");
          return removeMiddleContainerResult.Rv();
        }
        firstContent = lastContent = blockElement = nullptr;
      }
      if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        continue;
      }
      // Remove current block
      nsresult rv = RemoveBlockContainerWithTransaction(
          MOZ_KnownLive(*content->AsElement()));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return rv;
      }
      continue;
    }

    // XXX How about, <th>, <thead>, <tfoot>, <dt>, <dl>?
    if (content->IsAnyOfHTMLElements(
            nsGkAtoms::table, nsGkAtoms::tr, nsGkAtoms::tbody, nsGkAtoms::td,
            nsGkAtoms::li, nsGkAtoms::blockquote, nsGkAtoms::div) ||
        HTMLEditUtils::IsAnyListElement(content)) {
      // Process any partial progress saved
      if (blockElement) {
        SplitRangeOffFromNodeResult removeMiddleContainerResult =
            SplitRangeOffFromBlockAndRemoveMiddleContainer(
                *blockElement, *firstContent, *lastContent);
        if (removeMiddleContainerResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer() "
              "failed");
          return removeMiddleContainerResult.Rv();
        }
        firstContent = lastContent = blockElement = nullptr;
      }
      if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        continue;
      }
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      nsresult rv = RemoveBlockContainerElements(childContents);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerElements() failed");
        return rv;
      }
      continue;
    }

    if (HTMLEditUtils::IsInlineElement(content)) {
      if (blockElement) {
        // If so, is this node a descendant?
        if (EditorUtils::IsDescendantOf(*content, *blockElement)) {
          // Then we don't need to do anything different for this node
          lastContent = content;
          continue;
        }
        // Otherwise, we have progressed beyond end of blockElement, so let's
        // handle it now.  We need to remove the portion of blockElement that
        // contains [firstContent - lastContent].
        SplitRangeOffFromNodeResult removeMiddleContainerResult =
            SplitRangeOffFromBlockAndRemoveMiddleContainer(
                *blockElement, *firstContent, *lastContent);
        if (removeMiddleContainerResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer() "
              "failed");
          return removeMiddleContainerResult.Rv();
        }
        firstContent = lastContent = blockElement = nullptr;
        // Fall out and handle content
      }
      blockElement = HTMLEditUtils::GetAncestorBlockElement(content);
      if (!blockElement || !HTMLEditUtils::IsFormatNode(blockElement) ||
          !EditorUtils::IsEditableContent(*blockElement, EditorType::HTML)) {
        // Not a block kind that we care about.
        blockElement = nullptr;
      } else {
        firstContent = lastContent = content;
      }
      continue;
    }

    if (blockElement) {
      // Some node that is already sans block style.  Skip over it and process
      // any partial progress saved.
      SplitRangeOffFromNodeResult removeMiddleContainerResult =
          SplitRangeOffFromBlockAndRemoveMiddleContainer(
              *blockElement, *firstContent, *lastContent);
      if (removeMiddleContainerResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer() "
            "failed");
        return removeMiddleContainerResult.Rv();
      }
      firstContent = lastContent = blockElement = nullptr;
      continue;
    }
  }
  // Process any partial progress saved
  if (blockElement) {
    SplitRangeOffFromNodeResult removeMiddleContainerResult =
        SplitRangeOffFromBlockAndRemoveMiddleContainer(
            *blockElement, *firstContent, *lastContent);
    if (removeMiddleContainerResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::SplitRangeOffFromBlockAndRemoveMiddleContainer() "
          "failed");
      return removeMiddleContainerResult.Rv();
    }
    firstContent = lastContent = blockElement = nullptr;
  }
  return NS_OK;
}

nsresult HTMLEditor::CreateOrChangeBlockContainerElement(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents, nsAtom& aBlockTag) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // Intent of this routine is to be used for converting to/from headers,
  // paragraphs, pre, and address.  Those blocks that pretty much just contain
  // inline things...
  nsCOMPtr<Element> newBlock;
  nsCOMPtr<Element> curBlock;
  for (auto& content : aArrayOfContents) {
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsInContentNode())) {
      // If given node has been removed from the document, let's ignore it
      // since the following code may need its parent replace it with new
      // block.
      curBlock = nullptr;
      newBlock = nullptr;
      continue;
    }

    // Is it already the right kind of block, or an uneditable block?
    if (content->IsHTMLElement(&aBlockTag) ||
        (!EditorUtils::IsEditableContent(content, EditorType::HTML) &&
         HTMLEditUtils::IsBlockElement(content))) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      // Do nothing to this block
      continue;
    }

    // If content is a address, p, header, address, or pre, replace it with a
    // new block of correct type.
    // XXX: pre can't hold everything the others can
    if (HTMLEditUtils::IsMozDiv(content) ||
        HTMLEditUtils::IsFormatNode(content)) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      newBlock = ReplaceContainerAndCloneAttributesWithTransaction(
          MOZ_KnownLive(*content->AsElement()), aBlockTag);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!newBlock) {
        NS_WARNING(
            "EditorBase::ReplaceContainerAndCloneAttributesWithTransaction() "
            "failed");
        return NS_ERROR_FAILURE;
      }
      // If the new block element was moved to different element or removed by
      // the web app via mutation event listener, we should stop handling this
      // action since we cannot handle each of a lot of edge cases.
      if (NS_WARN_IF(newBlock->GetParentNode() != atContent.GetContainer())) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      continue;
    }

    if (HTMLEditUtils::IsTable(content) ||
        HTMLEditUtils::IsAnyListElement(content) ||
        content->IsAnyOfHTMLElements(nsGkAtoms::tbody, nsGkAtoms::tr,
                                     nsGkAtoms::td, nsGkAtoms::li,
                                     nsGkAtoms::blockquote, nsGkAtoms::div)) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      if (!childContents.IsEmpty()) {
        nsresult rv =
            CreateOrChangeBlockContainerElement(childContents, aBlockTag);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::CreateOrChangeBlockContainerElement() failed");
          return rv;
        }
        continue;
      }

      // Make sure we can put a block here
      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(aBlockTag, atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
        return splitNodeResult.Rv();
      }
      // If current handling node has been moved from the container by a
      // mutation event listener when we need to do something more for it,
      // we should stop handling this action since we cannot handle each of
      // a lot of edge cases.
      if (NS_WARN_IF(atContent.HasChildMovedFromContainer())) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      EditorDOMPoint splitPoint = splitNodeResult.SplitPoint();
      RefPtr<Element> theBlock =
          CreateNodeWithTransaction(aBlockTag, splitPoint);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!theBlock) {
        NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }
      // If the new block element was moved to different element or removed by
      // the web app via mutation event listener, we should stop handling this
      // action since we cannot handle each of a lot of edge cases.
      if (NS_WARN_IF(theBlock->GetParentNode() != splitPoint.GetContainer())) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      // Remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = std::move(theBlock);
      continue;
    }

    if (content->IsHTMLElement(nsGkAtoms::br)) {
      // If the node is a break, we honor it by putting further nodes in a new
      // parent
      if (curBlock) {
        // Forget any previous block used for previous inline nodes
        curBlock = nullptr;
        // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
        // alive.
        nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
          return rv;
        }
        continue;
      }

      // The break is the first (or even only) node we encountered.  Create a
      // block for it.
      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(aBlockTag, atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
        return splitNodeResult.Rv();
      }
      // If current handling node has been moved from the container by a
      // mutation event listener when we need to do something more for it,
      // we should stop handling this action since we cannot handle each of
      // a lot of edge cases.
      if (NS_WARN_IF(atContent.HasChildMovedFromContainer())) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      EditorDOMPoint splitPoint = splitNodeResult.SplitPoint();
      curBlock = CreateNodeWithTransaction(aBlockTag, splitPoint);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!curBlock) {
        NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }
      // If the new block element was moved to different element or removed by
      // the web app via mutation event listener, we should stop handling this
      // action since we cannot handle each of a lot of edge cases.
      if (NS_WARN_IF(curBlock->GetParentNode() != splitPoint.GetContainer())) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      // Remember our new block for postprocessing
      TopLevelEditSubActionDataRef().mNewBlockElement = curBlock;
      // Note: doesn't matter if we set mNewBlockElement multiple times.
      //
      // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
      // alive.
      nsresult rv =
          MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curBlock);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return rv;
      }
      continue;
    }

    if (HTMLEditUtils::IsInlineElement(content)) {
      // If content is inline, pull it into curBlock.  Note: it's assumed that
      // consecutive inline nodes in aNodeArray are actually members of the
      // same block parent.  This happens to be true now as a side effect of
      // how aNodeArray is contructed, but some additional logic should be
      // added here if that should change
      //
      // If content is a non editable, drop it if we are going to <pre>.
      if (&aBlockTag == nsGkAtoms::pre &&
          !EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        // Do nothing to this block
        continue;
      }

      // If no curBlock, make one
      if (!curBlock) {
        SplitNodeResult splitNodeResult =
            MaybeSplitAncestorsForInsertWithTransaction(aBlockTag, atContent);
        if (splitNodeResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() "
              "failed");
          return splitNodeResult.Rv();
        }
        // If current handling node has been moved from the container by a
        // mutation event listener when we need to do something more for it,
        // we should stop handling this action since we cannot handle each of
        // a lot of edge cases.
        if (NS_WARN_IF(atContent.HasChildMovedFromContainer())) {
          return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
        }
        EditorDOMPoint splitPoint = splitNodeResult.SplitPoint();
        curBlock = CreateNodeWithTransaction(aBlockTag, splitPoint);
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (!curBlock) {
          NS_WARNING("EditorBase::CreateNodeWithTransaction() failed");
          return NS_ERROR_FAILURE;
        }
        // If the new block element was moved to different element or removed
        // by the web app via mutation event listener, we should stop handling
        // this action since we cannot handle each of a lot of edge cases.
        if (NS_WARN_IF(curBlock->GetParentNode() !=
                       splitPoint.GetContainer())) {
          return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
        }

        // Update container of content.
        atContent.Set(content);

        // Remember our new block for postprocessing
        TopLevelEditSubActionDataRef().mNewBlockElement = curBlock;
        // Note: doesn't matter if we set mNewBlockElement multiple times.
      }

      if (NS_WARN_IF(!atContent.IsSet())) {
        // This is possible due to mutation events, let's not assert
        return NS_ERROR_UNEXPECTED;
      }

      // XXX If content is a br, replace it with a return if going to <pre>

      // This is a continuation of some inline nodes that belong together in
      // the same block item.  Use curBlock.
      //
      // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
      // alive.  We could try to make that a rvalue ref and create a const array
      // on the stack here, but callers are passing in auto arrays, and we don't
      // want to introduce copies..
      nsresult rv =
          MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curBlock);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return rv;
      }
    }
  }
  return NS_OK;
}

SplitNodeResult HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(
    nsAtom& aTag, const EditorDOMPoint& aStartOfDeepestRightNode) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(!aStartOfDeepestRightNode.IsSet())) {
    return SplitNodeResult(NS_ERROR_INVALID_ARG);
  }
  MOZ_ASSERT(aStartOfDeepestRightNode.IsSetAndValid());

  RefPtr<Element> host = GetActiveEditingHost();
  if (NS_WARN_IF(!host)) {
    return SplitNodeResult(NS_ERROR_FAILURE);
  }

  // The point must be descendant of editing host.
  if (aStartOfDeepestRightNode.GetContainer() != host &&
      !EditorUtils::IsDescendantOf(*aStartOfDeepestRightNode.GetContainer(),
                                   *host)) {
    NS_WARNING("aStartOfDeepestRightNode was not in editing host");
    return SplitNodeResult(NS_ERROR_INVALID_ARG);
  }

  // Look for a node that can legally contain the tag.
  EditorDOMPoint pointToInsert(aStartOfDeepestRightNode);
  for (; pointToInsert.IsSet();
       pointToInsert.Set(pointToInsert.GetContainer())) {
    // We cannot split active editing host and its ancestor.  So, there is
    // no element to contain the specified element.
    if (pointToInsert.GetChild() == host) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() reached "
          "editing host");
      return SplitNodeResult(NS_ERROR_FAILURE);
    }

    if (HTMLEditUtils::CanNodeContain(*pointToInsert.GetContainer(), aTag)) {
      // Found an ancestor node which can contain the element.
      break;
    }
  }

  MOZ_DIAGNOSTIC_ASSERT(pointToInsert.IsSet());

  // If the point itself can contain the tag, we don't need to split any
  // ancestor nodes.  In this case, we should return the given split point
  // as is.
  if (pointToInsert.GetContainer() == aStartOfDeepestRightNode.GetContainer()) {
    return SplitNodeResult(aStartOfDeepestRightNode);
  }

  SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
      MOZ_KnownLive(*pointToInsert.GetChild()), aStartOfDeepestRightNode,
      SplitAtEdges::eAllowToCreateEmptyContainer);
  if (NS_WARN_IF(Destroyed())) {
    return SplitNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(splitNodeResult.Succeeded(),
                       "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
                       "eAllowToCreateEmptyContainer) failed");
  return splitNodeResult;
}

nsresult HTMLEditor::JoinNearestEditableNodesWithTransaction(
    nsIContent& aNodeLeft, nsIContent& aNodeRight,
    EditorDOMPoint* aNewFirstChildOfRightNode) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aNewFirstChildOfRightNode);

  // Caller responsible for left and right node being the same type
  if (NS_WARN_IF(!aNodeLeft.GetParentNode())) {
    return NS_ERROR_FAILURE;
  }
  // If they don't have the same parent, first move the right node to after
  // the left one
  if (aNodeLeft.GetParentNode() != aNodeRight.GetParentNode()) {
    nsresult rv =
        MoveNodeWithTransaction(aNodeRight, EditorDOMPoint(&aNodeLeft));
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return rv;
    }
  }

  EditorDOMPoint ret(&aNodeRight, aNodeLeft.Length());

  // Separate join rules for differing blocks
  if (HTMLEditUtils::IsAnyListElement(&aNodeLeft) || aNodeLeft.IsText()) {
    // For lists, merge shallow (wouldn't want to combine list items)
    nsresult rv = JoinNodesWithTransaction(aNodeLeft, aNodeRight);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::JoinNodesWithTransaction failed");
    if (NS_SUCCEEDED(rv)) {
      // XXX `ret` may point invalid point if mutation event listener changed
      //     the previous siblings...
      *aNewFirstChildOfRightNode = std::move(ret);
    }
    return rv;
  }

  // Remember the last left child, and first right child
  nsCOMPtr<nsIContent> lastLeft = GetLastEditableChild(aNodeLeft);
  if (NS_WARN_IF(!lastLeft)) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIContent> firstRight = GetFirstEditableChild(aNodeRight);
  if (NS_WARN_IF(!firstRight)) {
    return NS_ERROR_FAILURE;
  }

  // For list items, divs, etc., merge smart
  nsresult rv = JoinNodesWithTransaction(aNodeLeft, aNodeRight);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::JoinNodesWithTransaction() failed");
    return rv;
  }

  if ((lastLeft->IsText() || lastLeft->IsElement()) &&
      HTMLEditUtils::CanContentsBeJoined(*lastLeft, *firstRight,
                                         StyleDifference::CompareIfElements)) {
    nsresult rv = JoinNearestEditableNodesWithTransaction(
        *lastLeft, *firstRight, aNewFirstChildOfRightNode);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::JoinNearestEditableNodesWithTransaction() failed");
    return rv;
  }
  // XXX `ret` may point invalid point if mutation event listener changed
  //     the previous siblings...
  *aNewFirstChildOfRightNode = std::move(ret);
  return NS_OK;
}

Element* HTMLEditor::GetMostAncestorMailCiteElement(nsINode& aNode) const {
  Element* mailCiteElement = nullptr;
  bool isPlaintextEditor = IsPlaintextEditor();
  for (nsINode* node = &aNode; node; node = node->GetParentNode()) {
    if ((isPlaintextEditor && node->IsHTMLElement(nsGkAtoms::pre)) ||
        HTMLEditUtils::IsMailCite(node)) {
      mailCiteElement = node->AsElement();
    }
    if (node->IsHTMLElement(nsGkAtoms::body)) {
      break;
    }
  }
  return mailCiteElement;
}

nsresult HTMLEditor::CacheInlineStyles(nsIContent& aContent) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv = GetInlineStyles(
      aContent, *TopLevelEditSubActionDataRef().mCachedInlineStyles);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::GetInlineStyles() failed");
  return rv;
}

nsresult HTMLEditor::GetInlineStyles(nsIContent& aContent,
                                     AutoStyleCacheArray& aStyleCacheArray) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aStyleCacheArray.IsEmpty());

  bool useCSS = IsCSSEnabled();

  for (nsStaticAtom* property :
       {nsGkAtoms::b, nsGkAtoms::i, nsGkAtoms::u, nsGkAtoms::face,
        nsGkAtoms::size, nsGkAtoms::color, nsGkAtoms::tt, nsGkAtoms::em,
        nsGkAtoms::strong, nsGkAtoms::dfn, nsGkAtoms::code, nsGkAtoms::samp,
        nsGkAtoms::var, nsGkAtoms::cite, nsGkAtoms::abbr, nsGkAtoms::acronym,
        nsGkAtoms::backgroundColor, nsGkAtoms::sub, nsGkAtoms::sup}) {
    nsStaticAtom *tag, *attribute;
    if (property == nsGkAtoms::face || property == nsGkAtoms::size ||
        property == nsGkAtoms::color) {
      tag = nsGkAtoms::font;
      attribute = property;
    } else {
      tag = property;
      attribute = nullptr;
    }
    // If type-in state is set, don't intervene
    bool typeInSet, unused;
    mTypeInState->GetTypingState(typeInSet, unused, tag, attribute, nullptr);
    if (typeInSet) {
      continue;
    }
    bool isSet = false;
    nsString value;  // Don't use nsAutoString here because it requires memcpy
                     // at creating new StyleCache instance.
    // Don't use CSS for <font size>, we don't support it usefully (bug 780035)
    if (!useCSS || (property == nsGkAtoms::size)) {
      isSet = IsTextPropertySetByContent(&aContent, tag, attribute, nullptr,
                                         &value);
    } else {
      isSet = CSSEditUtils::IsComputedCSSEquivalentToHTMLInlineStyleSet(
          aContent, MOZ_KnownLive(tag), MOZ_KnownLive(attribute), value);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
    }
    if (isSet) {
      aStyleCacheArray.AppendElement(StyleCache(tag, attribute, value));
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::ReapplyCachedStyles() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // The idea here is to examine our cached list of styles and see if any have
  // been removed.  If so, add typeinstate for them, so that they will be
  // reinserted when new content is added.

  if (TopLevelEditSubActionDataRef().mCachedInlineStyles->IsEmpty() ||
      !SelectionRefPtr()->RangeCount()) {
    return NS_OK;
  }

  // remember if we are in css mode
  bool useCSS = IsCSSEnabled();

  const RangeBoundary& atStartOfSelection =
      SelectionRefPtr()->GetRangeAt(0)->StartRef();
  nsCOMPtr<nsIContent> startContainerContent =
      atStartOfSelection.Container() &&
              atStartOfSelection.Container()->IsContent()
          ? atStartOfSelection.Container()->AsContent()
          : nullptr;
  if (NS_WARN_IF(!startContainerContent)) {
    return NS_OK;
  }

  AutoStyleCacheArray styleCacheArrayAtInsertionPoint;
  nsresult rv =
      GetInlineStyles(*startContainerContent, styleCacheArrayAtInsertionPoint);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::GetInlineStyles() failed, but ignored");
    return NS_OK;
  }

  for (StyleCache& styleCacheBeforeEdit :
       *TopLevelEditSubActionDataRef().mCachedInlineStyles) {
    bool isFirst = false, isAny = false, isAll = false;
    nsAutoString currentValue;
    if (useCSS) {
      // check computed style first in css case
      isAny = CSSEditUtils::IsComputedCSSEquivalentToHTMLInlineStyleSet(
          *startContainerContent, MOZ_KnownLive(styleCacheBeforeEdit.Tag()),
          MOZ_KnownLive(styleCacheBeforeEdit.GetAttribute()), currentValue);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
    }
    if (!isAny) {
      // then check typeinstate and html style
      nsresult rv = GetInlinePropertyBase(
          MOZ_KnownLive(*styleCacheBeforeEdit.Tag()),
          MOZ_KnownLive(styleCacheBeforeEdit.GetAttribute()),
          &styleCacheBeforeEdit.Value(), &isFirst, &isAny, &isAll,
          &currentValue);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::GetInlinePropertyBase() failed");
        return rv;
      }
    }
    // This style has disappeared through deletion.  Let's add the styles to
    // mTypeInState when same style isn't applied to the node already.
    if (isAny && !IsStyleCachePreservingSubAction(GetTopLevelEditSubAction())) {
      continue;
    }
    AutoStyleCacheArray::index_type index =
        styleCacheArrayAtInsertionPoint.IndexOf(
            styleCacheBeforeEdit.Tag(), styleCacheBeforeEdit.GetAttribute());
    if (index == AutoStyleCacheArray::NoIndex ||
        styleCacheBeforeEdit.Value() !=
            styleCacheArrayAtInsertionPoint.ElementAt(index).Value()) {
      mTypeInState->SetProp(styleCacheBeforeEdit.Tag(),
                            styleCacheBeforeEdit.GetAttribute(),
                            styleCacheBeforeEdit.Value());
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange(
    const RawRangeBoundary& aStartRef, const RawRangeBoundary& aEndRef) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoTArray<OwningNonNull<Element>, 64> arrayOfEmptyElements;
  DOMIterator iter;
  if (NS_FAILED(iter.Init(aStartRef, aEndRef))) {
    NS_WARNING("DOMIterator::Init() failed");
    return NS_ERROR_FAILURE;
  }
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void* aSelf) {
        MOZ_ASSERT(Element::FromNode(&aNode));
        MOZ_ASSERT(aSelf);
        Element* element = aNode.AsElement();
        if (!EditorUtils::IsEditableContent(*element, EditorType::HTML) ||
            (!HTMLEditUtils::IsListItem(element) &&
             !HTMLEditUtils::IsTableCellOrCaption(*element))) {
          return false;
        }
        return static_cast<HTMLEditor*>(aSelf)->IsEmptyNode(*element, false,
                                                            false);
      },
      arrayOfEmptyElements, this);

  // Put padding <br> elements for empty <li> and <td>.
  for (auto& emptyElement : arrayOfEmptyElements) {
    // Need to put br at END of node.  It may have empty containers in it and
    // still pass the "IsEmptyNode" test, and we want the br's to be after
    // them.  Also, we want the br to be after the selection if the selection
    // is in this node.
    EditorDOMPoint endOfNode(EditorDOMPoint::AtEndOf(emptyElement));
    CreateElementResult createPaddingBRResult =
        InsertPaddingBRElementForEmptyLastLineWithTransaction(endOfNode);
    if (createPaddingBRResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
          "failed");
      return createPaddingBRResult.Rv();
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::EnsureCaretInBlockElement(Element& aElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRefPtr()->IsCollapsed());

  EditorRawDOMPoint atCaret(EditorBase::GetStartPoint(*SelectionRefPtr()));
  if (NS_WARN_IF(!atCaret.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // Use ranges and RangeUtils::CompareNodeToRange() to compare selection
  // start to new block.
  RefPtr<StaticRange> staticRange =
      StaticRange::Create(atCaret.ToRawRangeBoundary(),
                          atCaret.ToRawRangeBoundary(), IgnoreErrors());
  if (!staticRange) {
    NS_WARNING("StaticRange::Create() failed");
    return NS_ERROR_FAILURE;
  }

  bool nodeBefore, nodeAfter;
  nsresult rv = RangeUtils::CompareNodeToRange(&aElement, staticRange,
                                               &nodeBefore, &nodeAfter);
  if (NS_FAILED(rv)) {
    NS_WARNING("RangeUtils::CompareNodeToRange() failed");
    return rv;
  }

  if (nodeBefore && nodeAfter) {
    return NS_OK;  // selection is inside block
  }

  if (nodeBefore) {
    // selection is after block.  put at end of block.
    nsIContent* lastEditableContent = GetLastEditableChild(aElement);
    if (!lastEditableContent) {
      lastEditableContent = &aElement;
    }
    EditorRawDOMPoint endPoint;
    if (lastEditableContent->IsText() ||
        HTMLEditUtils::IsContainerNode(*lastEditableContent)) {
      endPoint.SetToEndOf(lastEditableContent);
    } else {
      endPoint.SetAfter(lastEditableContent);
      if (NS_WARN_IF(!endPoint.IsSet())) {
        return NS_ERROR_FAILURE;
      }
    }
    nsresult rv = CollapseSelectionTo(endPoint);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionTo() failed");
    return rv;
  }

  // selection is before block.  put at start of block.
  nsIContent* firstEditableContent = GetFirstEditableChild(aElement);
  if (!firstEditableContent) {
    firstEditableContent = &aElement;
  }
  EditorRawDOMPoint atStartOfBlock;
  if (firstEditableContent->IsText() ||
      HTMLEditUtils::IsContainerNode(*firstEditableContent)) {
    atStartOfBlock.Set(firstEditableContent);
  } else {
    atStartOfBlock.Set(firstEditableContent, 0);
  }
  rv = CollapseSelectionTo(atStartOfBlock);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

void HTMLEditor::SetSelectionInterlinePosition() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRefPtr()->IsCollapsed());

  // Get the (collapsed) selection location
  const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return;
  }

  EditorDOMPoint atCaret(firstRange->StartRef());
  if (NS_WARN_IF(!atCaret.IsSet())) {
    return;
  }
  MOZ_ASSERT(atCaret.IsSetAndValid());

  // First, let's check to see if we are after a `<br>`.  We take care of this
  // special-case first so that we don't accidentally fall through into one of
  // the other conditionals.
  // XXX Although I don't understand "interline position", if caret is
  //     immediately after non-editable contents, but previous editable
  //     content is `<br>`, does this do right thing?
  if (nsIContent* previousEditableContentInBlock =
          GetPreviousEditableHTMLNodeInBlock(atCaret)) {
    if (previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::br)) {
      IgnoredErrorResult ignoredError;
      SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
      NS_WARNING_ASSERTION(
          !ignoredError.Failed(),
          "Selection::SetInterlinePosition(true) failed, but ignored");
      return;
    }
  }

  if (!atCaret.GetChild()) {
    return;
  }

  // If caret is immediately after a block, set interline position to "right".
  // XXX Although I don't understand "interline position", if caret is
  //     immediately after non-editable contents, but previous editable
  //     content is a block, does this do right thing?
  if (nsIContent* previousEditableContentInBlockAtCaret =
          GetPriorHTMLSibling(atCaret.GetChild())) {
    if (HTMLEditUtils::IsBlockElement(*previousEditableContentInBlockAtCaret)) {
      IgnoredErrorResult ignoredError;
      SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
      NS_WARNING_ASSERTION(
          !ignoredError.Failed(),
          "Selection::SetInterlinePosition(true) failed, but ignored");
      return;
    }
  }

  // If caret is immediately before a block, set interline position to "left".
  // XXX Although I don't understand "interline position", if caret is
  //     immediately before non-editable contents, but next editable
  //     content is a block, does this do right thing?
  if (nsIContent* nextEditableContentInBlockAtCaret =
          GetNextHTMLSibling(atCaret.GetChild())) {
    if (HTMLEditUtils::IsBlockElement(*nextEditableContentInBlockAtCaret)) {
      IgnoredErrorResult ignoredError;
      SelectionRefPtr()->SetInterlinePosition(false, ignoredError);
      NS_WARNING_ASSERTION(
          !ignoredError.Failed(),
          "Selection::SetInterlinePosition(false) failed, but ignored");
    }
  }
}

nsresult HTMLEditor::AdjustCaretPositionAndEnsurePaddingBRElement(
    nsIEditor::EDirection aDirectionAndAmount) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRefPtr()->IsCollapsed());

  EditorDOMPoint point(EditorBase::GetStartPoint(*SelectionRefPtr()));
  if (NS_WARN_IF(!point.IsInContentNode())) {
    return NS_ERROR_FAILURE;
  }

  // If selection start is not editable, climb up the tree until editable one.
  while (!EditorUtils::IsEditableContent(*point.ContainerAsContent(),
                                         EditorType::HTML)) {
    point.Set(point.GetContainer());
    if (NS_WARN_IF(!point.IsInContentNode())) {
      return NS_ERROR_FAILURE;
    }
  }

  // If caret is in empty block element, we need to insert a `<br>` element
  // because the block should have one-line height.
  if (RefPtr<Element> blockElement =
          HTMLEditUtils::GetInclusiveAncestorBlockElement(
              *point.ContainerAsContent())) {
    if (blockElement &&
        EditorUtils::IsEditableContent(*blockElement, EditorType::HTML) &&
        IsEmptyNode(*blockElement, false, false) &&
        HTMLEditUtils::CanNodeContain(*point.GetContainer(), *nsGkAtoms::br)) {
      Element* bodyOrDocumentElement = GetRoot();
      if (NS_WARN_IF(!bodyOrDocumentElement)) {
        return NS_ERROR_FAILURE;
      }
      if (point.GetContainer() == bodyOrDocumentElement) {
        // Our root node is completely empty. Don't add a <br> here.
        // AfterEditInner() will add one for us when it calls
        // TextEditor::MaybeCreatePaddingBRElementForEmptyEditor().
        // XXX This kind of dependency between methods makes us spaghetti.
        //     Let's handle it here later.
        // XXX This looks odd check.  If active editing host is not a
        //     `<body>`, what are we doing?
        return NS_OK;
      }
      CreateElementResult createPaddingBRResult =
          InsertPaddingBRElementForEmptyLastLineWithTransaction(point);
      NS_WARNING_ASSERTION(
          createPaddingBRResult.Succeeded(),
          "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
          "failed");
      return createPaddingBRResult.Rv();
    }
  }

  // XXX Perhaps, we should do something if we're in a data node but not
  //     a text node.
  if (point.IsInTextNode()) {
    return NS_OK;
  }

  // Do we need to insert a padding <br> element for empty last line?  We do
  // if we are:
  // 1) prior node is in same block where selection is AND
  // 2) prior node is a br AND
  // 3) that br is not visible

  if (nsCOMPtr<nsIContent> previousEditableContent =
          GetPreviousEditableHTMLNode(point)) {
    RefPtr<Element> blockElementAtCaret =
        HTMLEditUtils::GetInclusiveAncestorBlockElement(
            *point.ContainerAsContent());
    RefPtr<Element> blockElementParentAtPreviousEditableContent =
        HTMLEditUtils::GetAncestorBlockElement(*previousEditableContent);
    // If previous editable content of caret is in same block and a `<br>`
    // element, we need to adjust interline position.
    if (blockElementAtCaret &&
        blockElementAtCaret == blockElementParentAtPreviousEditableContent &&
        previousEditableContent &&
        previousEditableContent->IsHTMLElement(nsGkAtoms::br)) {
      // If it's an invisible `<br>` element, we need to insert a padding
      // `<br>` element for making empty line have one-line height.
      if (!IsVisibleBRElement(previousEditableContent)) {
        CreateElementResult createPaddingBRResult =
            InsertPaddingBRElementForEmptyLastLineWithTransaction(point);
        if (createPaddingBRResult.Failed()) {
          NS_WARNING(
              "HTMLEditor::"
              "InsertPaddingBRElementForEmptyLastLineWithTransaction() failed");
          return createPaddingBRResult.Rv();
        }
        point.Set(createPaddingBRResult.GetNewNode());
        // Selection stays *before* padding `<br>` element for empty last
        // line, sticking to it.
        IgnoredErrorResult ignoredError;
        SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        NS_WARNING_ASSERTION(
            !ignoredError.Failed(),
            "Selection::SetInterlinePosition(true) failed, but ignored");
        nsresult rv = CollapseSelectionTo(point);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::CollapseSelectionTo() failed");
          return rv;
        }
      }
      // If it's a visible `<br>` element and next editable content is a
      // padding `<br>` element, we need to set interline position.
      else if (nsIContent* nextEditableContentInBlock =
                   GetNextEditableHTMLNodeInBlock(*previousEditableContent)) {
        if (EditorUtils::IsPaddingBRElementForEmptyLastLine(
                *nextEditableContentInBlock)) {
          // Make it stick to the padding `<br>` element so that it will be
          // on blank line.
          IgnoredErrorResult ignoredError;
          SelectionRefPtr()->SetInterlinePosition(true, ignoredError);
          NS_WARNING_ASSERTION(
              !ignoredError.Failed(),
              "Selection::SetInterlinePosition(true) failed, but ignored");
        }
      }
    }
  }

  // If previous editable content in same block is `<br>`, text node, `<img>`
  //  or `<hr>`, current caret position is fine.
  if (nsIContent* previousEditableContentInBlock =
          GetPreviousEditableHTMLNodeInBlock(point)) {
    if (previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::br) ||
        previousEditableContentInBlock->IsText() ||
        HTMLEditUtils::IsImage(previousEditableContentInBlock) ||
        previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::hr)) {
      return NS_OK;
    }
  }
  // If next editable content in same block is `<br>`, text node, `<img>` or
  // `<hr>`, current caret position is fine.
  if (nsIContent* nextEditableContentInBlock =
          GetNextEditableHTMLNodeInBlock(point)) {
    if (nextEditableContentInBlock->IsText() ||
        nextEditableContentInBlock->IsAnyOfHTMLElements(
            nsGkAtoms::br, nsGkAtoms::img, nsGkAtoms::hr)) {
      return NS_OK;
    }
  }

  // Otherwise, look for a near editable content towards edit action direction.

  // If there is no editable content, keep current caret position.
  nsIContent* nearEditableContent =
      FindNearEditableContent(point, aDirectionAndAmount);
  if (!nearEditableContent) {
    return NS_OK;
  }

  EditorDOMPoint pointToPutCaret =
      GetGoodCaretPointFor(*nearEditableContent, aDirectionAndAmount);
  if (!pointToPutCaret.IsSet()) {
    NS_WARNING("HTMLEditor::GetGoodCaretPointFor() failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv = CollapseSelectionTo(pointToPutCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::CollapseSelectionTo() failed");
  return rv;
}

template <typename PT, typename CT>
nsIContent* HTMLEditor::FindNearEditableContent(
    const EditorDOMPointBase<PT, CT>& aPoint,
    nsIEditor::EDirection aDirection) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPoint.IsSetAndValid());

  nsIContent* editableContent = nullptr;
  if (aDirection == nsIEditor::ePrevious) {
    editableContent = GetPreviousEditableHTMLNode(aPoint);
    if (!editableContent) {
      return nullptr;  // Not illegal.
    }
  } else {
    editableContent = GetNextEditableHTMLNode(aPoint);
    if (NS_WARN_IF(!editableContent)) {
      // Perhaps, illegal because the node pointed by aPoint isn't editable
      // and nobody of previous nodes is editable.
      return nullptr;
    }
  }

  // scan in the right direction until we find an eligible text node,
  // but don't cross any breaks, images, or table elements.
  // XXX This comment sounds odd.  editableContent may have already crossed
  //     breaks and/or images if they are non-editable.
  while (editableContent && !editableContent->IsText() &&
         !editableContent->IsHTMLElement(nsGkAtoms::br) &&
         !HTMLEditUtils::IsImage(editableContent)) {
    if (aDirection == nsIEditor::ePrevious) {
      editableContent = GetPreviousEditableHTMLNode(*editableContent);
      if (NS_WARN_IF(!editableContent)) {
        return nullptr;
      }
    } else {
      editableContent = GetNextEditableHTMLNode(*editableContent);
      if (NS_WARN_IF(!editableContent)) {
        return nullptr;
      }
    }
  }

  // don't cross any table elements
  if (HTMLEditor::NodesInDifferentTableElements(*editableContent,
                                                *aPoint.GetContainer())) {
    return nullptr;
  }

  // otherwise, ok, we have found a good spot to put the selection
  return editableContent;
}

// static
bool HTMLEditor::NodesInDifferentTableElements(nsINode& aNode1,
                                               nsINode& aNode2) {
  nsINode* parentNode1;
  for (parentNode1 = &aNode1;
       parentNode1 && !HTMLEditUtils::IsAnyTableElement(parentNode1);
       parentNode1 = parentNode1->GetParentNode()) {
  }
  nsINode* parentNode2;
  for (parentNode2 = &aNode2;
       parentNode2 && !HTMLEditUtils::IsAnyTableElement(parentNode2);
       parentNode2 = parentNode2->GetParentNode()) {
  }
  // XXX Despite of the name, this returns true if only one node is in a
  //     table related element.
  return parentNode1 != parentNode2;
}

nsresult HTMLEditor::RemoveEmptyNodesIn(nsRange& aRange) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aRange.IsPositioned());

  // Some general notes on the algorithm used here: the goal is to examine all
  // the nodes in aRange, and remove the empty ones.  We do this by
  // using a content iterator to traverse all the nodes in the range, and
  // placing the empty nodes into an array.  After finishing the iteration,
  // we delete the empty nodes in the array.  (They cannot be deleted as we
  // find them because that would invalidate the iterator.)
  //
  // Since checking to see if a node is empty can be costly for nodes with
  // many descendants, there are some optimizations made.  I rely on the fact
  // that the iterator is post-order: it will visit children of a node before
  // visiting the parent node.  So if I find that a child node is not empty, I
  // know that its parent is not empty without even checking.  So I put the
  // parent on a "skipList" which is just a voidArray of nodes I can skip the
  // empty check on.  If I encounter a node on the skiplist, i skip the
  // processing for that node and replace its slot in the skiplist with that
  // node's parent.
  //
  // An interesting idea is to go ahead and regard parent nodes that are NOT
  // on the skiplist as being empty (without even doing the IsEmptyNode check)
  // on the theory that if they weren't empty, we would have encountered a
  // non-empty child earlier and thus put this parent node on the skiplist.
  //
  // Unfortunately I can't use that strategy here, because the range may
  // include some children of a node while excluding others.  Thus I could
  // find all the _examined_ children empty, but still not have an empty
  // parent.

  PostContentIterator postOrderIter;
  nsresult rv = postOrderIter.Init(&aRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("PostContentIterator::Init() failed");
    return rv;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfEmptyContents,
      arrayOfEmptyCites, skipList;

  // Check for empty nodes
  for (; !postOrderIter.IsDone(); postOrderIter.Next()) {
    MOZ_ASSERT(postOrderIter.GetCurrentNode()->IsContent());

    nsIContent* content = postOrderIter.GetCurrentNode()->AsContent();
    nsIContent* parentContent = content->GetParent();

    size_t idx = skipList.IndexOf(content);
    if (idx != skipList.NoIndex) {
      // This node is on our skip list.  Skip processing for this node, and
      // replace its value in the skip list with the value of its parent
      if (parentContent) {
        skipList[idx] = parentContent;
      }
      continue;
    }

    bool isCandidate = false;
    bool isMailCite = false;
    if (content->IsElement()) {
      if (content->IsHTMLElement(nsGkAtoms::body)) {
        // Don't delete the body
      } else if ((isMailCite = HTMLEditUtils::IsMailCite(content)) ||
                 content->IsHTMLElement(nsGkAtoms::a) ||
                 HTMLEditUtils::IsInlineStyle(content) ||
                 HTMLEditUtils::IsAnyListElement(content) ||
                 content->IsHTMLElement(nsGkAtoms::div)) {
        // Only consider certain nodes to be empty for purposes of removal
        isCandidate = true;
      } else if (HTMLEditUtils::IsFormatNode(content) ||
                 HTMLEditUtils::IsListItem(content) ||
                 content->IsHTMLElement(nsGkAtoms::blockquote)) {
        // These node types are candidates if selection is not in them.  If
        // it is one of these, don't delete if selection inside.  This is so
        // we can create empty headings, etc., for the user to type into.
        isCandidate = !StartOrEndOfSelectionRangesIsIn(*content);
      }
    }

    bool isEmptyNode = false;
    if (isCandidate) {
      // We delete mailcites even if they have a solo br in them.  Other
      // nodes we require to be empty.
      isEmptyNode = IsEmptyNode(*content, isMailCite, true);
      if (isEmptyNode) {
        if (isMailCite) {
          // mailcites go on a separate list from other empty nodes
          arrayOfEmptyCites.AppendElement(*content);
        } else {
          arrayOfEmptyContents.AppendElement(*content);
        }
      }
    }

    if (!isEmptyNode && parentContent) {
      // put parent on skip list
      skipList.AppendElement(*parentContent);
    }
  }

  // now delete the empty nodes
  for (OwningNonNull<nsIContent>& emptyContent : arrayOfEmptyContents) {
    // XXX Shouldn't we check whether it's removable from its parent??
    if (HTMLEditUtils::IsSimplyEditableNode(emptyContent)) {
      // MOZ_KnownLive because 'arrayOfEmptyContents' is guaranteed to keep it
      // alive.
      rv = DeleteNodeWithTransaction(MOZ_KnownLive(emptyContent));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
    }
  }

  // Now delete the empty mailcites.  This is a separate step because we want
  // to pull out any br's and preserve them.
  for (OwningNonNull<nsIContent>& emptyCite : arrayOfEmptyCites) {
    if (!IsEmptyNode(emptyCite, false, true)) {
      // We are deleting a cite that has just a `<br>`.  We want to delete cite,
      // but preserve `<br>`.
      RefPtr<Element> brElement =
          InsertBRElementWithTransaction(EditorDOMPoint(emptyCite));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!brElement) {
        NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
        return NS_ERROR_FAILURE;
      }
    }
    // MOZ_KnownLive because 'arrayOfEmptyCites' is guaranteed to keep it alive.
    rv = DeleteNodeWithTransaction(MOZ_KnownLive(emptyCite));
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  return NS_OK;
}

bool HTMLEditor::StartOrEndOfSelectionRangesIsIn(nsIContent& aContent) const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  for (uint32_t i = 0; i < SelectionRefPtr()->RangeCount(); ++i) {
    const nsRange* range = SelectionRefPtr()->GetRangeAt(i);
    nsINode* startContainer = range->GetStartContainer();
    if (startContainer) {
      if (&aContent == startContainer) {
        return true;
      }
      if (EditorUtils::IsDescendantOf(*startContainer, aContent)) {
        return true;
      }
    }
    nsINode* endContainer = range->GetEndContainer();
    if (startContainer == endContainer) {
      continue;
    }
    if (endContainer) {
      if (&aContent == endContainer) {
        return true;
      }
      if (EditorUtils::IsDescendantOf(*endContainer, aContent)) {
        return true;
      }
    }
  }
  return false;
}

nsresult HTMLEditor::LiftUpListItemElement(
    Element& aListItemElement,
    LiftUpFromAllParentListElements aLiftUpFromAllParentListElements) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsListItem(&aListItemElement)) {
    return NS_ERROR_INVALID_ARG;
  }

  if (NS_WARN_IF(!aListItemElement.GetParentElement()) ||
      NS_WARN_IF(!aListItemElement.GetParentElement()->GetParentNode())) {
    return NS_ERROR_FAILURE;
  }

  // if it's first or last list item, don't need to split the list
  // otherwise we do.
  bool isFirstListItem = IsFirstEditableChild(&aListItemElement);
  bool isLastListItem = IsLastEditableChild(&aListItemElement);

  Element* leftListElement = aListItemElement.GetParentElement();
  if (NS_WARN_IF(!leftListElement)) {
    return NS_ERROR_FAILURE;
  }

  // If it's at middle of parent list element, split the parent list element.
  // Then, aListItem becomes the first list item of the right list element.
  if (!isFirstListItem && !isLastListItem) {
    EditorDOMPoint atListItemElement(&aListItemElement);
    if (NS_WARN_IF(!atListItemElement.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    MOZ_ASSERT(atListItemElement.IsSetAndValid());
    ErrorResult error;
    nsCOMPtr<nsIContent> maybeLeftListContent =
        SplitNodeWithTransaction(atListItemElement, error);
    if (NS_WARN_IF(Destroyed())) {
      error = NS_ERROR_EDITOR_DESTROYED;
    }
    if (error.Failed()) {
      NS_WARNING_ASSERTION(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED),
                           "HTMLEditor::SplitNodeWithTransaction() failed");
      return error.StealNSResult();
    }
    if (!maybeLeftListContent->IsElement()) {
      NS_WARNING(
          "HTMLEditor::SplitNodeWithTransaction() didn't return left list "
          "element");
      return NS_ERROR_FAILURE;
    }
    leftListElement = maybeLeftListContent->AsElement();
  }

  // In most cases, insert the list item into the new left list node..
  EditorDOMPoint pointToInsertListItem(leftListElement);
  if (NS_WARN_IF(!pointToInsertListItem.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // But when the list item was the first child of the right list, it should
  // be inserted between the both list elements.  This allows user to hit
  // Enter twice at a list item breaks the parent list node.
  if (!isFirstListItem) {
    DebugOnly<bool> advanced = pointToInsertListItem.AdvanceOffset();
    NS_WARNING_ASSERTION(advanced,
                         "Failed to advance offset to right list node");
  }

  nsresult rv =
      MoveNodeWithTransaction(aListItemElement, pointToInsertListItem);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
    return rv;
  }

  // Unwrap list item contents if they are no longer in a list
  // XXX If the parent list element is a child of another list element
  //     (although invalid tree), the list item element won't be unwrapped.
  //     That makes the parent ancestor element tree valid, but might be
  //     unexpected result.
  // XXX If aListItemElement is <dl> or <dd> and current parent is <ul> or <ol>,
  //     the list items won't be unwrapped.  If aListItemElement is <li> and its
  //     current parent is <dl>, there is same issue.
  if (!HTMLEditUtils::IsAnyListElement(pointToInsertListItem.GetContainer()) &&
      HTMLEditUtils::IsListItem(&aListItemElement)) {
    nsresult rv = RemoveBlockContainerWithTransaction(aListItemElement);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::RemoveBlockContainerWithTransaction() failed");
    return rv;
  }
  if (aLiftUpFromAllParentListElements == LiftUpFromAllParentListElements::No) {
    return NS_OK;
  }
  // XXX If aListItemElement is moved to unexpected element by mutation event
  //     listener, shouldn't we stop calling this?
  rv = LiftUpListItemElement(aListItemElement,
                             LiftUpFromAllParentListElements::Yes);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::LiftUpListItemElement("
                       "LiftUpFromAllParentListElements::Yes) failed");
  return rv;
}

nsresult HTMLEditor::DestroyListStructureRecursively(Element& aListElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));

  // XXX If mutation event listener inserts new child into `aListElement`,
  //     this becomes infinite loop so that we should set limit of the
  //     loop count from original child count.
  while (aListElement.GetFirstChild()) {
    OwningNonNull<nsIContent> child = *aListElement.GetFirstChild();

    if (HTMLEditUtils::IsListItem(child)) {
      // XXX Using LiftUpListItemElement() is too expensive for this purpose.
      //     Looks like the reason why this method uses it is, only this loop
      //     wants to work with first child of aListElement.  However, what it
      //     actually does is removing <li> as container.  Perhaps, we should
      //     decide destination first, and then, move contents in `child`.
      // XXX If aListElement is is a child of another list element (although
      //     it's invalid tree), this moves the list item to outside of
      //     aListElement's parent.  Is that really intentional behavior?
      nsresult rv = LiftUpListItemElement(
          MOZ_KnownLive(*child->AsElement()),
          HTMLEditor::LiftUpFromAllParentListElements::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":Yes) failed");
        return rv;
      }
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(child)) {
      nsresult rv =
          DestroyListStructureRecursively(MOZ_KnownLive(*child->AsElement()));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DestroyListStructureRecursively() failed");
        return rv;
      }
      continue;
    }

    // Delete any non-list items for now
    // XXX This is not HTML5 aware.  HTML5 allows all list elements to have
    //     <script> and <template> and <dl> element to have <div> to group
    //     some <dt> and <dd> elements.  So, this may break valid children.
    nsresult rv = DeleteNodeWithTransaction(*child);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  // Delete the now-empty list
  nsresult rv = RemoveBlockContainerWithTransaction(aListElement);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::RemoveBlockContainerWithTransaction() failed");
  return rv;
}

nsresult HTMLEditor::EnsureSelectionInBodyOrDocumentElement() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  RefPtr<Element> bodyOrDocumentElement = GetRoot();
  if (NS_WARN_IF(!bodyOrDocumentElement)) {
    return NS_ERROR_FAILURE;
  }

  EditorRawDOMPoint atCaret(EditorBase::GetStartPoint(*SelectionRefPtr()));
  if (NS_WARN_IF(!atCaret.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // XXX This does wrong things.  Web apps can put any elements as sibling
  //     of `<body>` element.  Therefore, this collapses `Selection` into
  //     the `<body>` element which `HTMLDocument.body` is set to.  So,
  //     this makes users impossible to modify content outside of the
  //     `<body>` element even if caret is in an editing host.

  // Check that selection start container is inside the <body> element.
  // XXXsmaug this code is insane.
  nsINode* temp = atCaret.GetContainer();
  while (temp && !temp->IsHTMLElement(nsGkAtoms::body)) {
    temp = temp->GetParentOrShadowHostNode();
  }

  // If we aren't in the <body> element, force the issue.
  if (!temp) {
    nsresult rv = CollapseSelectionToStartOf(*bodyOrDocumentElement);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionToStartOf() failed, but ignored");
    return NS_OK;
  }

  EditorRawDOMPoint selectionEndPoint(
      EditorBase::GetEndPoint(*SelectionRefPtr()));
  if (NS_WARN_IF(!selectionEndPoint.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // check that selNode is inside body
  // XXXsmaug this code is insane.
  temp = selectionEndPoint.GetContainer();
  while (temp && !temp->IsHTMLElement(nsGkAtoms::body)) {
    temp = temp->GetParentOrShadowHostNode();
  }

  // If we aren't in the <body> element, force the issue.
  if (!temp) {
    nsresult rv = CollapseSelectionToStartOf(*bodyOrDocumentElement);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::CollapseSelectionToStartOf() failed, but ignored");
  }

  return NS_OK;
}

nsresult HTMLEditor::InsertPaddingBRElementForEmptyLastLineIfNeeded(
    Element& aElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsBlockElement(aElement)) {
    return NS_OK;
  }

  if (!IsEmptyNode(aElement)) {
    return NS_OK;
  }

  CreateElementResult createBRResult =
      InsertPaddingBRElementForEmptyLastLineWithTransaction(
          EditorDOMPoint(&aElement, 0));
  NS_WARNING_ASSERTION(
      createBRResult.Succeeded(),
      "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
      "failed");
  return createBRResult.Rv();
}

nsresult HTMLEditor::InsertBRElementIfEmptyBlockElement(Element& aElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsBlockElement(aElement)) {
    return NS_OK;
  }

  if (!IsEmptyNode(aElement)) {
    return NS_OK;
  }

  RefPtr<Element> brElement =
      InsertBRElementWithTransaction(EditorDOMPoint(&aElement, 0));
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(brElement,
                       "HTMELditor::InsertBRElementWithTransaction() failed");
  return brElement ? NS_OK : NS_ERROR_FAILURE;
}

nsresult HTMLEditor::RemoveAlignFromDescendants(Element& aElement,
                                                const nsAString& aAlignType,
                                                EditTarget aEditTarget) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!aElement.IsHTMLElement(nsGkAtoms::table));

  bool useCSS = IsCSSEnabled();

  // Let's remove all alignment hints in the children of aNode; it can
  // be an ALIGN attribute (in case we just remove it) or a CENTER
  // element (here we have to remove the container and keep its
  // children). We break on tables and don't look at their children.
  nsCOMPtr<nsIContent> nextSibling;
  for (nsIContent* content =
           aEditTarget == EditTarget::NodeAndDescendantsExceptTable
               ? &aElement
               : aElement.GetFirstChild();
       content; content = nextSibling) {
    // Get the next sibling before removing content from the DOM tree.
    // XXX If next sibling is removed from the parent and/or inserted to
    //     different parent, we will behave unexpectedly.  I think that
    //     we should create child list and handle it with checking whether
    //     it's still a child of expected parent.
    nextSibling = aEditTarget == EditTarget::NodeAndDescendantsExceptTable
                      ? nullptr
                      : content->GetNextSibling();

    if (content->IsHTMLElement(nsGkAtoms::center)) {
      OwningNonNull<Element> centerElement = *content->AsElement();
      nsresult rv = RemoveAlignFromDescendants(
          centerElement, aAlignType, EditTarget::OnlyDescendantsExceptTable);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
            "OnlyDescendantsExceptTable) failed");
        return rv;
      }

      // We may have to insert a `<br>` element before first child of the
      // `<center>` element because it should be first element of a hard line
      // even after removing the `<center>` element.
      rv = EnsureHardLineBeginsWithFirstChildOf(centerElement);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::EnsureHardLineBeginsWithFirstChildOf() failed");
        return rv;
      }

      // We may have to insert a `<br>` element after last child of the
      // `<center>` element because it should be last element of a hard line
      // even after removing the `<center>` element.
      rv = EnsureHardLineEndsWithLastChildOf(centerElement);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::EnsureHardLineEndsWithLastChildOf() failed");
        return rv;
      }

      rv = RemoveContainerWithTransaction(centerElement);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveContainerWithTransaction() failed");
        return rv;
      }
      continue;
    }

    if (!HTMLEditUtils::IsBlockElement(*content) &&
        !content->IsHTMLElement(nsGkAtoms::hr)) {
      continue;
    }

    OwningNonNull<Element> blockOrHRElement = *content->AsElement();
    if (HTMLEditUtils::SupportsAlignAttr(blockOrHRElement)) {
      nsresult rv =
          RemoveAttributeWithTransaction(blockOrHRElement, *nsGkAtoms::align);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::align) "
            "failed");
        return rv;
      }
    }
    if (useCSS) {
      if (blockOrHRElement->IsAnyOfHTMLElements(nsGkAtoms::table,
                                                nsGkAtoms::hr)) {
        nsresult rv = SetAttributeOrEquivalent(
            blockOrHRElement, nsGkAtoms::align, aAlignType, false);
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
          return rv;
        }
      } else {
        nsStyledElement* styledBlockOrHRElement =
            nsStyledElement::FromNode(blockOrHRElement);
        if (NS_WARN_IF(!styledBlockOrHRElement)) {
          return NS_ERROR_FAILURE;
        }
        // MOZ_KnownLive(*styledBlockOrHRElement): It's `blockOrHRElement
        // which is OwningNonNull.
        nsAutoString dummyCssValue;
        nsresult rv = mCSSEditUtils->RemoveCSSInlineStyleWithTransaction(
            MOZ_KnownLive(*styledBlockOrHRElement), nsGkAtoms::textAlign,
            dummyCssValue);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "CSSEditUtils::RemoveCSSInlineStyleWithTransaction(nsGkAtoms::"
              "textAlign) failed");
          return rv;
        }
      }
    }
    if (!blockOrHRElement->IsHTMLElement(nsGkAtoms::table)) {
      // unless this is a table, look at children
      nsresult rv = RemoveAlignFromDescendants(
          blockOrHRElement, aAlignType, EditTarget::OnlyDescendantsExceptTable);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
            "OnlyDescendantsExceptTable) failed");
        return rv;
      }
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::EnsureHardLineBeginsWithFirstChildOf(
    Element& aRemovingContainerElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsIContent* firstEditableChild =
      GetFirstEditableChild(aRemovingContainerElement);
  if (!firstEditableChild) {
    return NS_OK;
  }

  if (HTMLEditUtils::IsBlockElement(*firstEditableChild) ||
      firstEditableChild->IsHTMLElement(nsGkAtoms::br)) {
    return NS_OK;
  }

  nsIContent* previousEditableContent =
      GetPriorHTMLSibling(&aRemovingContainerElement);
  if (!previousEditableContent) {
    return NS_OK;
  }

  if (HTMLEditUtils::IsBlockElement(*previousEditableContent) ||
      previousEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return NS_OK;
  }

  RefPtr<Element> brElement = InsertBRElementWithTransaction(
      EditorDOMPoint(&aRemovingContainerElement, 0));
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(brElement,
                       "HTMLEditor::InsertBRElementWithTransaction() failed");
  return brElement ? NS_OK : NS_ERROR_FAILURE;
}

nsresult HTMLEditor::EnsureHardLineEndsWithLastChildOf(
    Element& aRemovingContainerElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsIContent* firstEditableContent =
      GetLastEditableChild(aRemovingContainerElement);
  if (!firstEditableContent) {
    return NS_OK;
  }

  if (HTMLEditUtils::IsBlockElement(*firstEditableContent) ||
      firstEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return NS_OK;
  }

  nsIContent* nextEditableContent =
      GetPriorHTMLSibling(&aRemovingContainerElement);
  if (!nextEditableContent) {
    return NS_OK;
  }

  if (HTMLEditUtils::IsBlockElement(*nextEditableContent) ||
      nextEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return NS_OK;
  }

  RefPtr<Element> brElement = InsertBRElementWithTransaction(
      EditorDOMPoint::AtEndOf(aRemovingContainerElement));
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(brElement,
                       "HTMLEditor::InsertBRElementWithTransaction() failed");
  return brElement ? NS_OK : NS_ERROR_FAILURE;
}

nsresult HTMLEditor::SetBlockElementAlign(Element& aBlockOrHRElement,
                                          const nsAString& aAlignType,
                                          EditTarget aEditTarget) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsBlockElement(aBlockOrHRElement) ||
             aBlockOrHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(IsCSSEnabled() ||
             HTMLEditUtils::SupportsAlignAttr(aBlockOrHRElement));

  if (!aBlockOrHRElement.IsHTMLElement(nsGkAtoms::table)) {
    nsresult rv =
        RemoveAlignFromDescendants(aBlockOrHRElement, aAlignType, aEditTarget);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::RemoveAlignFromDescendants() failed");
      return rv;
    }
  }
  nsresult rv = SetAttributeOrEquivalent(&aBlockOrHRElement, nsGkAtoms::align,
                                         aAlignType, false);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
  return rv;
}

nsresult HTMLEditor::ChangeMarginStart(Element& aElement,
                                       ChangeMargin aChangeMargin) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsStaticAtom& marginProperty = MarginPropertyAtomForIndent(aElement);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  nsAutoString value;
  DebugOnly<nsresult> rvIgnored =
      CSSEditUtils::GetSpecifiedProperty(aElement, marginProperty, value);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rvIgnored),
      "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
  float f;
  RefPtr<nsAtom> unit;
  CSSEditUtils::ParseLength(value, &f, getter_AddRefs(unit));
  if (!f) {
    nsAutoString defaultLengthUnit;
    CSSEditUtils::GetDefaultLengthUnit(defaultLengthUnit);
    unit = NS_Atomize(defaultLengthUnit);
  }
  int8_t multiplier = aChangeMargin == ChangeMargin::Increase ? 1 : -1;
  if (nsGkAtoms::in == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_IN * multiplier;
  } else if (nsGkAtoms::cm == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_CM * multiplier;
  } else if (nsGkAtoms::mm == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_MM * multiplier;
  } else if (nsGkAtoms::pt == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PT * multiplier;
  } else if (nsGkAtoms::pc == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PC * multiplier;
  } else if (nsGkAtoms::em == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_EM * multiplier;
  } else if (nsGkAtoms::ex == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_EX * multiplier;
  } else if (nsGkAtoms::px == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PX * multiplier;
  } else if (nsGkAtoms::percentage == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PERCENT * multiplier;
  }

  if (0 < f) {
    if (nsStyledElement* styledElement = nsStyledElement::FromNode(&aElement)) {
      nsAutoString newValue;
      newValue.AppendFloat(f);
      newValue.Append(nsDependentAtomString(unit));
      // MOZ_KnownLive(*styledElement): It's aElement and its lifetime must
      // be guaranteed by caller because of MOZ_CAN_RUN_SCRIPT method.
      // MOZ_KnownLive(merginProperty): It's nsStaticAtom.
      nsresult rv = mCSSEditUtils->SetCSSPropertyWithTransaction(
          MOZ_KnownLive(*styledElement), MOZ_KnownLive(marginProperty),
          newValue);
      if (rv == NS_ERROR_EDITOR_DESTROYED) {
        NS_WARNING(
            "CSSEditUtils::SetCSSPropertyWithTransaction() destroyed the "
            "editor");
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "CSSEditUtils::SetCSSPropertyWithTransaction() failed, but ignored");
    }
    return NS_OK;
  }

  if (nsStyledElement* styledElement = nsStyledElement::FromNode(&aElement)) {
    // MOZ_KnownLive(*styledElement): It's aElement and its lifetime must
    // be guaranteed by caller because of MOZ_CAN_RUN_SCRIPT method.
    // MOZ_KnownLive(merginProperty): It's nsStaticAtom.
    nsresult rv = mCSSEditUtils->RemoveCSSPropertyWithTransaction(
        MOZ_KnownLive(*styledElement), MOZ_KnownLive(marginProperty), value);
    if (rv == NS_ERROR_EDITOR_DESTROYED) {
      NS_WARNING(
          "CSSEditUtils::RemoveCSSPropertyWithTransaction() destroyed the "
          "editor");
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "CSSEditUtils::RemoveCSSPropertyWithTransaction() failed, but ignored");
  }

  // Remove unnecessary divs
  if (!aElement.IsHTMLElement(nsGkAtoms::div) ||
      HTMLEditor::HasAttributes(&aElement)) {
    return NS_OK;
  }
  // Don't touch editiong host nor node which is outside of it.
  Element* editingHost = GetActiveEditingHost();
  if (&aElement == editingHost ||
      !aElement.IsInclusiveDescendantOf(editingHost)) {
    return NS_OK;
  }

  nsresult rv = RemoveContainerWithTransaction(aElement);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::RemoveContainerWithTransaction() failed");
  return rv;
}

EditActionResult HTMLEditor::SetSelectionToAbsoluteAsSubAction() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetPositionToAbsolute, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditgor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Element> focusElement = GetSelectionContainerElement();
  if (focusElement && HTMLEditUtils::IsImage(focusElement)) {
    TopLevelEditSubActionDataRef().mNewBlockElement = std::move(focusElement);
    return EditActionHandled();
  }

  if (!SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = MaybeExtendSelectionToHardLineEdgesForBlockEditAction();
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::MaybeExtendSelectionToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionHandled(rv);
    }
  }

  // XXX Is this right thing?
  TopLevelEditSubActionDataRef().mNewBlockElement = nullptr;

  RefPtr<Element> divElement;
  rv = MoveSelectedContentsToDivElementToMakeItAbsolutePosition(
      address_of(divElement));
  // MoveSelectedContentsToDivElementToMakeItAbsolutePosition() may restore
  // selection with AutoSelectionRestorer.  Therefore, the editor might have
  // already been destroyed now.
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::MoveSelectedContentsToDivElementToMakeItAbsolutePosition()"
        " failed");
    return EditActionHandled(rv);
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return EditActionHandled(rv);
  }

  if (!divElement) {
    return EditActionHandled();
  }

  rv = SetPositionToAbsoluteOrStatic(*divElement, true);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::SetPositionToAbsoluteOrStatic() failed");

  TopLevelEditSubActionDataRef().mNewBlockElement = std::move(divElement);
  return EditActionHandled(rv);
}

nsresult HTMLEditor::MoveSelectedContentsToDivElementToMakeItAbsolutePosition(
    RefPtr<Element>* aTargetElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aTargetElement);

  AutoSelectionRestorer restoreSelectionLater(*this);

  AutoTArray<RefPtr<nsRange>, 4> arrayOfRanges;
  GetSelectionRangesExtendedToHardLineStartAndEnd(
      arrayOfRanges, EditSubAction::eSetPositionToAbsolute);

  // Use these ranges to construct a list of nodes to act on.
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  nsresult rv = SplitInlinesAndCollectEditTargetNodes(
      arrayOfRanges, arrayOfContents, EditSubAction::eSetPositionToAbsolute,
      CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::SplitInlinesAndCollectEditTargetNodes("
        "eSetPositionToAbsolute, CollectNonEditableNodes::Yes) failed");
    return rv;
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (IsEmptyOneHardLine(arrayOfContents)) {
    const nsRange* firstRange = SelectionRefPtr()->GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return NS_ERROR_FAILURE;
    }

    EditorDOMPoint atCaret(firstRange->StartRef());
    if (NS_WARN_IF(!atCaret.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // Make sure we can put a block here.
    SplitNodeResult splitNodeResult =
        MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::div, atCaret);
    if (splitNodeResult.Failed()) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms::"
          "div) failed");
      return splitNodeResult.Rv();
    }
    RefPtr<Element> newDivElement = CreateNodeWithTransaction(
        *nsGkAtoms::div, splitNodeResult.SplitPoint());
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (!newDivElement) {
      NS_WARNING(
          "EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
      return NS_ERROR_FAILURE;
    }
    // Delete anything that was in the list of nodes
    // XXX We don't need to remove items from the array.
    while (!arrayOfContents.IsEmpty()) {
      OwningNonNull<nsIContent>& curNode = arrayOfContents[0];
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*curNode));
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteNodeWithTransaction() failed");
        return rv;
      }
      arrayOfContents.RemoveElementAt(0);
    }
    // Don't restore the selection
    restoreSelectionLater.Abort();
    nsresult rv = CollapseSelectionToStartOf(*newDivElement);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::CollapseSelectionToStartOf() failed");
    *aTargetElement = std::move(newDivElement);
    return rv;
  }

  // `<div>` element to be positioned absolutely.  This may have already
  // existed or newly created by this method.
  RefPtr<Element> targetDivElement;
  // Newly created list element for moving selected list item elements into
  // targetDivElement.  I.e., this is created in the `<div>` element.
  RefPtr<Element> createdListElement;
  // If we handle a parent list item element, this is set to it.  In such case,
  // we should handle its children again.
  RefPtr<Element> handledListItemElement;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      return NS_ERROR_FAILURE;  // XXX not continue??
    }

    // Ignore all non-editable nodes.  Leave them be.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    // If current node is a child of a list element, we need another list
    // element in absolute-positioned `<div>` element to avoid non-selected
    // list items are moved into the `<div>` element.
    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      // If we cannot move current node to created list element, we need a
      // list element in the target `<div>` element for the destination.
      // Therefore, duplicate same list element into the target `<div>`
      // element.
      nsIContent* previousEditableContent =
          createdListElement ? GetPriorHTMLSibling(content) : nullptr;
      if (!createdListElement ||
          (previousEditableContent &&
           previousEditableContent != createdListElement)) {
        nsAtom* ULOrOLOrDLTagName =
            atContent.GetContainer()->NodeInfo()->NameAtom();
        SplitNodeResult splitNodeResult =
            MaybeSplitAncestorsForInsertWithTransaction(
                MOZ_KnownLive(*ULOrOLOrDLTagName), atContent);
        if (NS_WARN_IF(splitNodeResult.Failed())) {
          return splitNodeResult.Rv();
        }
        if (!targetDivElement) {
          targetDivElement = CreateNodeWithTransaction(
              *nsGkAtoms::div, splitNodeResult.SplitPoint());
          if (NS_WARN_IF(Destroyed())) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          if (NS_WARN_IF(!targetDivElement)) {
            return NS_ERROR_FAILURE;
          }
        }
        createdListElement = CreateNodeWithTransaction(
            MOZ_KnownLive(*ULOrOLOrDLTagName),
            EditorDOMPoint::AtEndOf(targetDivElement));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_WARN_IF(!createdListElement)) {
          return NS_ERROR_FAILURE;
        }
      }
      // Move current node (maybe, assumed as a list item element) into the
      // new list element in the target `<div>` element to be positioned
      // absolutely.
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
      nsresult rv = MoveNodeToEndWithTransaction(MOZ_KnownLive(content),
                                                 *createdListElement);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      continue;
    }

    // If contents in a list item element is selected, we should move current
    // node into the target `<div>` element with the list item element itself
    // because we want to keep indent level of the contents.
    if (RefPtr<Element> listItemElement =
            GetNearestAncestorListItemElement(content)) {
      if (handledListItemElement == listItemElement) {
        // Current node has already been moved into the `<div>` element.
        continue;
      }
      // If we cannot move the list item element into created list element,
      // we need another list element in the target `<div>` element.
      nsIContent* previousEditableContent =
          createdListElement ? GetPriorHTMLSibling(listItemElement) : nullptr;
      if (!createdListElement ||
          (previousEditableContent &&
           previousEditableContent != createdListElement)) {
        EditorDOMPoint atListItem(listItemElement);
        if (NS_WARN_IF(!atListItem.IsSet())) {
          return NS_ERROR_FAILURE;
        }
        // XXX If content is the listItemElement and not in a list element,
        //     we duplicate wrong element into the target `<div>` element.
        nsAtom* containerName =
            atListItem.GetContainer()->NodeInfo()->NameAtom();
        SplitNodeResult splitNodeResult =
            MaybeSplitAncestorsForInsertWithTransaction(
                MOZ_KnownLive(*containerName), atListItem);
        if (NS_WARN_IF(splitNodeResult.Failed())) {
          return splitNodeResult.Rv();
        }
        if (!targetDivElement) {
          targetDivElement = CreateNodeWithTransaction(
              *nsGkAtoms::div, EditorDOMPoint(atListItem.GetContainer()));
          if (NS_WARN_IF(Destroyed())) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          if (NS_WARN_IF(!targetDivElement)) {
            return NS_ERROR_FAILURE;
          }
        }
        // XXX So, createdListElement may be set to a non-list element.
        createdListElement = CreateNodeWithTransaction(
            MOZ_KnownLive(*containerName),
            EditorDOMPoint::AtEndOf(targetDivElement));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_WARN_IF(!createdListElement)) {
          return NS_ERROR_FAILURE;
        }
      }
      // Move current list item element into the createdListElement (could be
      // non-list element due to the above bug) in a candidate `<div>` element
      // to be positioned absolutely.
      nsresult rv =
          MoveNodeToEndWithTransaction(*listItemElement, *createdListElement);
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return rv;
      }
      handledListItemElement = std::move(listItemElement);
      continue;
    }

    if (!targetDivElement) {
      // If we meet a `<div>` element, use it as the absolute-position
      // container.
      // XXX This looks odd.  If there are 2 or more `<div>` elements are
      //     selected, first found `<div>` element will have all other
      //     selected nodes.
      if (content->IsHTMLElement(nsGkAtoms::div)) {
        targetDivElement = content->AsElement();
        MOZ_ASSERT(!createdListElement);
        MOZ_ASSERT(!handledListItemElement);
        continue;
      }
      // Otherwise, create new `<div>` element to be positioned absolutely
      // and to contain all selected nodes.
      SplitNodeResult splitNodeResult =
          MaybeSplitAncestorsForInsertWithTransaction(*nsGkAtoms::div,
                                                      atContent);
      if (splitNodeResult.Failed()) {
        NS_WARNING(
            "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(nsGkAtoms:"
            ":div) failed");
        return splitNodeResult.Rv();
      }
      targetDivElement = CreateNodeWithTransaction(
          *nsGkAtoms::div, splitNodeResult.SplitPoint());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (!targetDivElement) {
        NS_WARNING(
            "EditorBase::CreateNodeWithTransaction(nsGkAtoms::div) failed");
        return NS_ERROR_FAILURE;
      }
    }

    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
    nsresult rv =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *targetDivElement);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return rv;
    }
    // Forget createdListElement, if any
    createdListElement = nullptr;
  }
  *aTargetElement = std::move(targetDivElement);
  return NS_OK;
}

EditActionResult HTMLEditor::SetSelectionToStaticAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetPositionToStatic, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Element> element = GetAbsolutelyPositionedSelectionContainer();
  if (!element) {
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING(
        "HTMLEditor::GetAbsolutelyPositionedSelectionContainer() returned "
        "nullptr");
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  {
    AutoSelectionRestorer restoreSelectionLater(*this);

    nsresult rv = SetPositionToAbsoluteOrStatic(*element, false);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::SetPositionToAbsoluteOrStatic() failed");
      return EditActionHandled(rv);
    }
  }

  // Restoring Selection might cause destroying the HTML editor.
  return NS_WARN_IF(Destroyed()) ? EditActionHandled(NS_ERROR_EDITOR_DESTROYED)
                                 : EditActionHandled(NS_OK);
}

EditActionResult HTMLEditor::AddZIndexAsSubAction(int32_t aChange) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(*this,
                                             ScrollSelectionIntoView::Yes);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this,
      aChange < 0 ? EditSubAction::eDecreaseZIndex
                  : EditSubAction::eIncreaseZIndex,
      nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRefPtr()->IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterPaddingBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterPaddingBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Element> absolutelyPositionedElement =
      GetAbsolutelyPositionedSelectionContainer();
  if (!absolutelyPositionedElement) {
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING(
        "HTMLEditor::GetAbsolutelyPositionedSelectionContainer() returned "
        "nullptr");
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  nsStyledElement* absolutelyPositionedStyledElement =
      nsStyledElement::FromNode(absolutelyPositionedElement);
  if (NS_WARN_IF(!absolutelyPositionedStyledElement)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  {
    AutoSelectionRestorer restoreSelectionLater(*this);

    // MOZ_KnownLive(*absolutelyPositionedStyledElement): It's
    // absolutelyPositionedElement whose type is RefPtr.
    Result<int32_t, nsresult> result = AddZIndexWithTransaction(
        MOZ_KnownLive(*absolutelyPositionedStyledElement), aChange);
    if (result.isErr()) {
      NS_WARNING("HTMLEditor::AddZIndexWithTransaction() failed");
      return EditActionHandled(result.unwrapErr());
    }
  }

  // Restoring Selection might cause destroying the HTML editor.
  return NS_WARN_IF(Destroyed()) ? EditActionHandled(NS_ERROR_EDITOR_DESTROYED)
                                 : EditActionHandled(NS_OK);
}

nsresult HTMLEditor::OnDocumentModified() {
  if (mPendingDocumentModifiedRunner) {
    return NS_OK;  // We've already posted same runnable into the queue.
  }
  mPendingDocumentModifiedRunner = NewRunnableMethod(
      "HTMLEditor::OnModifyDocument", this, &HTMLEditor::OnModifyDocument);
  nsContentUtils::AddScriptRunner(do_AddRef(mPendingDocumentModifiedRunner));
  // Be aware, if OnModifyDocument() may be called synchronously, the
  // editor might have been destroyed here.
  return NS_WARN_IF(Destroyed()) ? NS_ERROR_EDITOR_DESTROYED : NS_OK;
}

}  // namespace mozilla
