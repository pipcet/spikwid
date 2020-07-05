/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"
#include "Http3Session.h"
#include "Http3Stream.h"
#include "nsHttpRequestHead.h"
#include "nsISocketTransport.h"
#include "nsSocketTransportService2.h"

#include <stdio.h>

namespace mozilla {
namespace net {

Http3Stream::Http3Stream(nsAHttpTransaction* httpTransaction,
                         Http3Session* session)
    : mSendState(PREPARING_HEADERS),
      mRecvState(READING_HEADERS),
      mStreamId(UINT64_MAX),
      mSession(session),
      mTransaction(httpTransaction),
      mQueued(false),
      mRequestBlockedOnRead(false),
      mDataReceived(false),
      mResetRecv(false),
      mRequestBodyLenRemaining(0),
      mSocketTransport(session->SocketTransport()),
      mTotalSent(0),
      mTotalRead(0),
      mFin(false) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG3(("Http3Stream::Http3Stream [this=%p]", this));
}

void Http3Stream::Close(nsresult aResult) { mTransaction->Close(aResult); }

bool Http3Stream::GetHeadersString(const char* buf, uint32_t avail,
                                   uint32_t* countUsed) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG3(("Http3Stream::GetHeadersString %p avail=%u.", this, avail));

  mFlatHttpRequestHeaders.Append(buf, avail);
  // We can use the simple double crlf because firefox is the
  // only client we are parsing
  int32_t endHeader = mFlatHttpRequestHeaders.Find("\r\n\r\n");

  if (endHeader == kNotFound) {
    // We don't have all the headers yet
    LOG3(
        ("Http3Stream::GetHeadersString %p "
         "Need more header bytes. Len = %u",
         this, mFlatHttpRequestHeaders.Length()));
    *countUsed = avail;
    return false;
  }

  uint32_t oldLen = mFlatHttpRequestHeaders.Length();
  mFlatHttpRequestHeaders.SetLength(endHeader + 2);
  *countUsed = avail - (oldLen - endHeader) + 4;

  FindRequestContentLength();
  return true;
}

void Http3Stream::FindRequestContentLength() {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  // Look for Content-Length header to find out if we have request body and
  // how long it is.
  int32_t contentLengthStart = mFlatHttpRequestHeaders.Find("Content-Length:");
  if (contentLengthStart == -1) {
    // There is no content-Length.
    return;
  }

  // We have Content-Length header, find the end of it.
  int32_t crlfIndex =
      mFlatHttpRequestHeaders.Find("\r\n", false, contentLengthStart);
  if (crlfIndex == -1) {
    MOZ_ASSERT(false, "We must have \\r\\n at the end of the headers string.");
    return;
  }

  // Find the beginning.
  int32_t valueIndex =
      mFlatHttpRequestHeaders.Find(":", false, contentLengthStart) + 1;
  if (valueIndex > crlfIndex) {
    // Content-Length headers is empty.
    MOZ_ASSERT(false, "Content-Length must have a value.");
    return;
  }

  const char* beginBuffer = mFlatHttpRequestHeaders.BeginReading();
  while (valueIndex < crlfIndex && beginBuffer[valueIndex] == ' ') {
    ++valueIndex;
  }

  nsDependentCSubstring value =
      Substring(beginBuffer + valueIndex, beginBuffer + crlfIndex);

  int64_t len;
  nsCString tmp(value);
  if (nsHttp::ParseInt64(tmp.get(), nullptr, &len)) {
    mRequestBodyLenRemaining = len;
  }
}

nsresult Http3Stream::TryActivating() {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG(("Http3Stream::TryActivating [this=%p]", this));
  nsHttpRequestHead* head = mTransaction->RequestHead();

  nsAutoCString authorityHeader;
  nsresult rv = head->GetHeader(nsHttp::Host, authorityHeader);
  if (NS_FAILED(rv)) {
    MOZ_ASSERT(false);
    return rv;
  }

  nsDependentCString scheme(head->IsHTTPS() ? "https" : "http");

  nsAutoCString method;
  nsAutoCString path;
  head->Method(method);
  head->Path(path);

  return mSession->TryActivating(method, scheme, authorityHeader, path,
                                 mFlatHttpRequestHeaders, &mStreamId, this);
}

nsresult Http3Stream::OnReadSegment(const char* buf, uint32_t count,
                                    uint32_t* countRead) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  LOG(("Http3Stream::OnReadSegment count=%u state=%d [this=%p]", count,
       mSendState, this));

  nsresult rv = NS_OK;

  switch (mSendState) {
    case PREPARING_HEADERS: {
      bool done = GetHeadersString(buf, count, countRead);

      if (*countRead) {
        mTotalSent += *countRead;
      }

      if (!done) {
        break;
      }
      mSendState = WAITING_TO_ACTIVATE;
    }
      [[fallthrough]];
    case WAITING_TO_ACTIVATE:
      rv = TryActivating();
      if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
        LOG3(("Http3Stream::OnReadSegment %p cannot activate now. queued.\n",
              this));
        rv = *countRead ? NS_OK : NS_BASE_STREAM_WOULD_BLOCK;
        break;
      }
      if (NS_FAILED(rv)) {
        LOG3(("Http3Stream::OnReadSegment %p cannot activate error=0x%" PRIx32
              ".",
              this, static_cast<uint32_t>(rv)));
        break;
      }

      // Successfully activated.
      mTransaction->OnTransportStatus(mSocketTransport,
                                      NS_NET_STATUS_SENDING_TO, mTotalSent);

      if (mRequestBodyLenRemaining) {
        mSendState = SENDING_BODY;
      } else {
        mTransaction->OnTransportStatus(mSocketTransport,
                                        NS_NET_STATUS_WAITING_FOR, 0);
        mSession->CloseSendingSide(mStreamId);
        mSendState = SEND_DONE;
      }
      break;
    case SENDING_BODY: {
      rv = mSession->SendRequestBody(mStreamId, buf, count, countRead);
      MOZ_ASSERT(mRequestBodyLenRemaining >= *countRead,
                 "We cannot send more that than we promised.");
      if (mRequestBodyLenRemaining < *countRead) {
        rv = NS_ERROR_UNEXPECTED;
      }
      if (NS_FAILED(rv)) {
        LOG3(
            ("Http3Stream::OnReadSegment %p sending body returns "
             "error=0x%" PRIx32 ".",
             this, static_cast<uint32_t>(rv)));
        break;
      }

      mRequestBodyLenRemaining -= *countRead;
      if (!mRequestBodyLenRemaining) {
        mTransaction->OnTransportStatus(mSocketTransport,
                                        NS_NET_STATUS_WAITING_FOR, 0);
        mSession->CloseSendingSide(mStreamId);
        mSendState = SEND_DONE;
      }
    } break;
    case EARLY_RESPONSE:
      // We do not need to send the rest of the request, so just ignore it.
      *countRead = count;
      mRequestBodyLenRemaining -= count;
      if (!mRequestBodyLenRemaining) {
        mTransaction->OnTransportStatus(mSocketTransport,
                                        NS_NET_STATUS_WAITING_FOR, 0);
        mSendState = SEND_DONE;
      }
      break;
    default:
      MOZ_ASSERT(false, "We are done sending this request!");
      break;
  }

  if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
    mRequestBlockedOnRead = true;
  }
  return rv;
}

void Http3Stream::SetResponseHeaders(nsTArray<uint8_t>& aResponseHeaders,
                                     bool aFin) {
  MOZ_ASSERT(mFlatResponseHeaders.IsEmpty(),
             "Cannot set response headers more than once");
  mFlatResponseHeaders.SwapElements(aResponseHeaders);
  mFin = aFin;
}

nsresult Http3Stream::OnWriteSegment(char* buf, uint32_t count,
                                     uint32_t* countWritten) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  LOG(("Http3Stream::OnWriteSegment [this=%p, state=%d", this, mRecvState));
  nsresult rv = NS_OK;
  switch (mRecvState) {
    case READING_HEADERS: {
      // SetResponseHeaders should have been previously called.
      MOZ_ASSERT(!mFlatResponseHeaders.IsEmpty(), "Headers empty!");
      *countWritten = (mFlatResponseHeaders.Length() > count)
                          ? count
                          : mFlatResponseHeaders.Length();
      memcpy(buf, mFlatResponseHeaders.Elements(), *countWritten);

      mFlatResponseHeaders.RemoveElementsAt(0, *countWritten);
      if (mFlatResponseHeaders.Length() == 0) {
        mRecvState = mFin ? RECEIVED_FIN : READING_DATA;
      }

      if (*countWritten == 0) {
        rv = NS_BASE_STREAM_WOULD_BLOCK;
      } else {
        mTotalRead += *countWritten;
        mTransaction->OnTransportStatus(
            mSocketTransport, NS_NET_STATUS_RECEIVING_FROM, mTotalRead);
      }
    } break;
    case READING_DATA: {
      rv = mSession->ReadResponseData(mStreamId, buf, count, countWritten,
                                      &mFin);
      if (NS_FAILED(rv)) {
        return rv;
      }
      if (*countWritten == 0) {
        if (mFin) {
          mRecvState = RECV_DONE;
          rv = NS_BASE_STREAM_CLOSED;
        } else {
          rv = NS_BASE_STREAM_WOULD_BLOCK;
        }
      } else {
        mTotalRead += *countWritten;
        mTransaction->OnTransportStatus(
            mSocketTransport, NS_NET_STATUS_RECEIVING_FROM, mTotalRead);

        if (mFin) {
          mRecvState = RECEIVED_FIN;
        }
      }
    } break;
    case RECEIVED_FIN:
    case RECEIVED_RESET:
      rv = NS_BASE_STREAM_CLOSED;
      mRecvState = RECV_DONE;
      break;
    case RECV_DONE:
      rv = NS_ERROR_UNEXPECTED;
  }

  return rv;
}

nsresult Http3Stream::ReadSegments(nsAHttpSegmentReader* reader, uint32_t count,
                                   uint32_t* countRead) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  mRequestBlockedOnRead = false;

  if (mRecvState == RECV_DONE) {
    // Don't transmit any request frames if the peer cannot respond or respone
    // is already done.
    LOG3(
        ("Http3Stream %p ReadSegments request stream aborted due to"
         " response side closure\n",
         this));
    return NS_ERROR_ABORT;
  }

  nsresult rv = NS_OK;
  switch (mSendState) {
    case WAITING_TO_ACTIVATE: {
      *countRead = 0;
      // A transaction that had already generated its headers before it was
      // queued at the session level (due to concurrency concerns) may not call
      // onReadSegment off the ReadSegments() stack above.
      LOG3(("Http3Stream %p ReadSegments forcing OnReadSegment call\n", this));
      uint32_t wasted = 0;
      nsresult rv2 = OnReadSegment("", 0, &wasted);
      LOG3(("  OnReadSegment returned 0x%08" PRIx32,
            static_cast<uint32_t>(rv2)));
      if (mSendState != SENDING_BODY) {
        break;
      }
    }
      // If we are in state SENDING_BODY we can continue sending data.
      [[fallthrough]];
    case PREPARING_HEADERS:
    case SENDING_BODY: {
      rv = mTransaction->ReadSegments(this, count, countRead);
      LOG(("Http3Stream::ReadSegments rv=0x%" PRIx32 " [this=%p]",
           static_cast<uint32_t>(rv), this));

      if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
        mRequestBlockedOnRead = true;
      }
      if (NS_SUCCEEDED(rv) && mRequestBlockedOnRead) {
        // We've got NS_BASE_STREAM_WOULD_BLOCK in Http3Stream::OnReadSegment()
        // but the error code was lost in nsBufferedInputStream::ReadSegments().
        // Restore it here.
        rv = NS_BASE_STREAM_WOULD_BLOCK;
      }
    } break;
    default:
      *countRead = 0;
      rv = NS_OK;
      break;
  }
  LOG(("Http3Stream::ReadSegments rv=0x%" PRIx32 " [this=%p]",
       static_cast<uint32_t>(rv), this));
  return rv;
}

nsresult Http3Stream::WriteSegments(nsAHttpSegmentWriter* writer,
                                    uint32_t count, uint32_t* countWritten) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG(("Http3Stream::WriteSegments [this=%p]", this));
  nsresult rv = mTransaction->WriteSegments(this, count, countWritten);
  LOG(("Http3Stream::WriteSegments rv=0x%" PRIx32 " [this=%p]",
       static_cast<uint32_t>(rv), this));
  return rv;
}

}  // namespace net
}  // namespace mozilla
