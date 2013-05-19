/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2001
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Vikas Arora <vikasa@google.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "ImageLogging.h"
#include "nsWEBPDecoder.h"

#include "nsIInputStream.h"

#include "nspr.h"
#include "nsCRT.h"
#include "gfxColor.h"

#include "gfxPlatform.h"

namespace mozilla {
namespace image {

#if defined(PR_LOGGING)
static PRLogModuleInfo *gWEBPLog = PR_NewLogModule("WEBPDecoder");
static PRLogModuleInfo *gWEBPDecoderAccountingLog =
                        PR_NewLogModule("WEBPDecoderAccounting");
#else
#define gWEBPlog
#define gWEBPDecoderAccountingLog
#endif

nsWEBPDecoder::nsWEBPDecoder(RasterImage &aImage)
 : Decoder(aImage)
{
  PR_LOG(gWEBPDecoderAccountingLog, PR_LOG_DEBUG,
         ("nsWEBPDecoder::nsWEBPDecoder: Creating WEBP decoder %p",
          this));
}

nsWEBPDecoder::~nsWEBPDecoder()
{
  PR_LOG(gWEBPDecoderAccountingLog, PR_LOG_DEBUG,
         ("nsWEBPDecoder::~nsWEBPDecoder: Destroying WEBP decoder %p",
          this));
}


void
nsWEBPDecoder::InitInternal()
{
  if (!WebPInitDecBuffer(&mDecBuf)) {
    PostDecoderError(NS_ERROR_FAILURE);
  }
  mDecBuf.colorspace = MODE_RGBA;
  mDecoder = WebPINewDecoder(&mDecBuf);
  if (!mDecoder) {
    PostDecoderError(NS_ERROR_FAILURE);
  }
}

void
nsWEBPDecoder::FinishInternal()
{
  // Flush the Decoder and let it free the output image buffer.
  WebPIDelete(mDecoder);
  WebPFreeDecBuffer(&mDecBuf);

  // We should never make multiple frames
  NS_ABORT_IF_FALSE(GetFrameCount() <= 1, "Multiple WebP frames?");

  // Send notifications if appropriate
  if (!IsSizeDecode() && (GetFrameCount() == 1)) {
    PostFrameStop();
    PostDecodeDone();
  }
}

void
nsWEBPDecoder::WriteInternal(const char *aBuffer, uint32_t aCount)
{
  NS_ABORT_IF_FALSE(!HasError(), "Shouldn't call WriteInternal after error!");

  const uint8_t* buf = (const uint8_t*)aBuffer;
  VP8StatusCode rv = WebPIAppend(mDecoder, buf, aCount);
  if (rv == VP8_STATUS_OUT_OF_MEMORY) {
    PostDecoderError(NS_ERROR_OUT_OF_MEMORY);
    return;
  } else if (rv == VP8_STATUS_INVALID_PARAM ||
             rv == VP8_STATUS_BITSTREAM_ERROR) {
    PostDataError();
    return;
  } else if (rv == VP8_STATUS_UNSUPPORTED_FEATURE ||
             rv == VP8_STATUS_USER_ABORT) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }

  // Catch any remaining erroneous return value.
  if (rv != VP8_STATUS_OK && rv != VP8_STATUS_SUSPENDED) {
    PostDecoderError(NS_ERROR_FAILURE);
  }

  int lastLineRead = -1;
  int height = 0;
  int width = 0;
  int stride = 0;

  mData = WebPIDecGetRGB(mDecoder, &lastLineRead, &width, &height, &stride);

  // WebP encoded Image Data hasn't read yet, return.
  if (lastLineRead == -1 || !mData)
    return;

  // Ensure valid Image height & width.
  if (width <= 0 || height <= 0) {
    PostDataError();
    return;
  }

  // Post our size to the superclass
  PostSize(width, height);

  // If we're doing a size decode, we're done.
  if (IsSizeDecode())
    return;

  PRUint32 imagelength;
  // First incremental Image data chunk. Special handling required.
  if (mLastLine == 0 && lastLineRead > 0) {
    imgFrame* aFrame;
    nsresult res = mImage.EnsureFrame(0, 0, 0, width, height,
                                       gfxASurface::ImageFormatARGB32,
                                       (uint8_t**)&mImageData, &imagelength, &aFrame);
    if (NS_FAILED(res) || !mImageData) {
      PostDecoderError(NS_ERROR_FAILURE);
      return;
    }
    // Tell the superclass we're starting a frame.
    PostFrameStart();
  }

  if (!mImageData) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }

  if (lastLineRead > mLastLine) {
    // PRUint32 bpr = width * sizeof(PRUint32);
    for (int line = mLastLine; line < lastLineRead; line++) {
      PRUint32 *cptr32 = (PRUint32*)(mImageData + (line * width));
      PRUint8  *cptr8 = mData + (line * stride);
      for (int pix = 0; pix < width; pix++, cptr8 += 4) {
        *cptr32++ = gfxPackedPixel(cptr8[3], cptr8[0], cptr8[1], cptr8[2]);
      }
    }

    // Invalidate
    nsIntRect r(0, mLastLine, width, lastLineRead);
    PostInvalidation(r);
  }

  mLastLine = lastLineRead;
  return;
}

} // namespace imagelib
} // namespace mozilla

