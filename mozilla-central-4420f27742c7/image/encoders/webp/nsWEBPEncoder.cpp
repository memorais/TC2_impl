/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCRT.h"
#include "nsWEBPEncoder.h"
#include "prprf.h"
#include "nsString.h"
#include "nsStreamUtils.h"

using namespace mozilla;

NS_IMPL_THREADSAFE_ISUPPORTS3(nsWEBPEncoder, imgIEncoder, nsIInputStream, nsIAsyncInputStream)

nsWEBPEncoder::nsWEBPEncoder() : picture(nullptr), config(nullptr), memory_writer(nullptr),
				 mFinished(false),
				 mImageBuffer(nullptr), mImageBufferSize(0),
                                 mImageBufferUsed(0), mImageBufferReadPoint(0),
                                 mCallback(nullptr),
                                 mCallbackTarget(nullptr), mNotifyThreshold(0),
                                 mReentrantMonitor("nsWEBPEncoder.mReentrantMonitor")
{
}

nsWEBPEncoder::~nsWEBPEncoder()
{
  if (mImageBuffer) {
    moz_free(mImageBuffer);
    mImageBuffer = nullptr;
  }
}

NS_IMETHODIMP nsWEBPEncoder::InitFromData(const uint8_t* aData,
                                          uint32_t aLength, // (unused, req'd by JS)
                                          uint32_t aWidth,
                                          uint32_t aHeight,
                                          uint32_t aStride,
                                          uint32_t aInputFormat,
                                          const nsAString& aOutputOptions)
{
  NS_ENSURE_ARG(aData);
  nsresult rv;

  rv = StartImageEncode(aWidth, aHeight, aInputFormat, aOutputOptions);
  if (!NS_SUCCEEDED(rv))
    return rv;

  rv = AddImageFrame(aData, aLength, aWidth, aHeight, aStride,
                     aInputFormat, aOutputOptions);
  if (!NS_SUCCEEDED(rv))
    return rv;

  rv = EndImageEncode();

  return rv;
}

NS_IMETHODIMP nsWEBPEncoder::StartImageEncode(uint32_t aWidth,
                                              uint32_t aHeight,
                                              uint32_t aInputFormat,
                                              const nsAString& aOutputOptions)
{
  // can't initialize more than once
  if (mImageBuffer != nullptr)
    return NS_ERROR_ALREADY_INITIALIZED;

  // validate input format
  if (aInputFormat != INPUT_FORMAT_RGB &&
      aInputFormat != INPUT_FORMAT_RGBA &&
      aInputFormat != INPUT_FORMAT_HOSTARGB)
    return NS_ERROR_INVALID_ARG;

  // Initializing webp needs
  /* WebPPicture picture;
  WebPConfig config;
  WebPMemoryWriter memory_writer; */
  
  WebPMemoryWriterInit(&memory_writer);
  
  // Checking initialization
  if (!WebPConfigInit(&config) || !WebPPictureInit(&picture))
	return NS_ERROR_FAILURE; 

  picture.width = aWidth;
  picture.height = aHeight;

  // Memory allocation
  // The memory will be freed on EndImageEncode
  if (!WebPPictureAlloc(&picture))
	return NS_ERROR_OUT_OF_MEMORY;
  
  // Setting our webp writer
  picture.writer = WebPMemoryWriter;
  picture.writer = &memory_writer;

  return NS_OK;

}

// Returns the number of bytes in the image buffer used.
NS_IMETHODIMP nsWEBPEncoder::GetImageBufferUsed(uint32_t *aOutputSize)
{
  NS_ENSURE_ARG_POINTER(aOutputSize);
  *aOutputSize = mImageBufferUsed;
  return NS_OK;
}

// Returns a pointer to the start of the image buffer
NS_IMETHODIMP nsWEBPEncoder::GetImageBuffer(char **aOutputBuffer)
{
  NS_ENSURE_ARG_POINTER(aOutputBuffer);
  *aOutputBuffer = reinterpret_cast<char*>(mImageBuffer);
  return NS_OK;
}

// TODO

NS_IMETHODIMP nsWEBPEncoder::AddImageFrame(const uint8_t* aData,
                                           uint32_t aLength,
                                           uint32_t aWidth,
                                           uint32_t aHeight,
                                           uint32_t aStride,
                                           uint32_t aFrameFormat,
                                           const nsAString& aFrameOptions)
{

  // must be initialized
  if (mImageBuffer == nullptr)
    return NS_ERROR_NOT_INITIALIZED;

  // validate input format
  if (aInputFormat != INPUT_FORMAT_RGB &&
      aInputFormat != INPUT_FORMAT_RGBA &&
      aInputFormat != INPUT_FORMAT_HOSTARGB)
    return NS_ERROR_INVALID_ARG;

  // Simple conversion first
  size_t buffSize = sizeof(aData);
  uint8_t* row = new uint8_t[aWidth * 4];
  for (uint32_t y = 0; y < aHeight; y ++) {
      WebPMemoryWrite((uint8_t*)&aData[y * aStride], buffSize, &picture);
  }

  return NS_OK;

}

NS_IMETHODIMP nsWEBPEncoder::EndImageEncode()
{

  // must be initialized
  if (mImageBuffer == nullptr)
    return NS_ERROR_NOT_INITIALIZED;

  // if output callback can't get enough memory, it will free our buffer
  if (!mImageBuffer)
    return NS_ERROR_OUT_OF_MEMORY;

  WebPPictureFree(&picture);

  mFinished = true;
  NotifyListener();

return NS_OK;

}

NS_IMETHODIMP nsWEBPEncoder::Close()
{
  if (mImageBuffer != nullptr) {
    moz_free(mImageBuffer);
    mImageBuffer = nullptr;
    mImageBufferSize = 0;
    mImageBufferUsed = 0;
    mImageBufferReadPoint = 0;
  }
  return NS_OK;
}

NS_IMETHODIMP nsWEBPEncoder::Available(uint64_t *_retval)
{
  if (!mImageBuffer)
    return NS_BASE_STREAM_CLOSED;

  *_retval = mImageBufferUsed - mImageBufferReadPoint;
  return NS_OK;
}

NS_IMETHODIMP nsWEBPEncoder::Read(char * aBuf, uint32_t aCount,
                                 uint32_t *_retval)
{
  return ReadSegments(NS_CopySegmentToBuffer, aBuf, aCount, _retval);
}

/* [noscript] unsigned long readSegments (in nsWriteSegmentFun aWriter, in voidPtr aClosure, in unsigned long aCount); */
NS_IMETHODIMP nsWEBPEncoder::ReadSegments(nsWriteSegmentFun aWriter, void *aClosure, uint32_t aCount, uint32_t *_retval)
{
  // Avoid another thread reallocing the buffer underneath us
  ReentrantMonitorAutoEnter autoEnter(mReentrantMonitor);

  uint32_t maxCount = mImageBufferUsed - mImageBufferReadPoint;
  if (maxCount == 0) {
    *_retval = 0;
    return mFinished ? NS_OK : NS_BASE_STREAM_WOULD_BLOCK;
  }

  if (aCount > maxCount)
    aCount = maxCount;
  nsresult rv = aWriter(this, aClosure,
                        reinterpret_cast<const char*>(mImageBuffer+mImageBufferReadPoint),
                        0, aCount, _retval);
  if (NS_SUCCEEDED(rv)) {
    NS_ASSERTION(*_retval <= aCount, "bad write count");
    mImageBufferReadPoint += *_retval;
  }

  // errors returned from the writer end here!
  return NS_OK;
}

NS_IMETHODIMP nsWEBPEncoder::IsNonBlocking(bool *_retval)
{
  *_retval = true;
  return NS_OK;
}

NS_IMETHODIMP nsWEBPEncoder::AsyncWait(nsIInputStreamCallback *aCallback,
                                      uint32_t aFlags,
                                      uint32_t aRequestedCount,
                                      nsIEventTarget *aTarget)
{
  if (aFlags != 0)
    return NS_ERROR_NOT_IMPLEMENTED;

  if (mCallback || mCallbackTarget)
    return NS_ERROR_UNEXPECTED;

  mCallbackTarget = aTarget;
  // 0 means "any number of bytes except 0"
  mNotifyThreshold = aRequestedCount;
  if (!aRequestedCount)
    mNotifyThreshold = 1024; // We don't want to notify incessantly

  // We set the callback absolutely last, because NotifyListener uses it to
  // determine if someone needs to be notified.  If we don't set it last,
  // NotifyListener might try to fire off a notification to a null target
  // which will generally cause non-threadsafe objects to be used off the main thread
  mCallback = aCallback;

  // What we are being asked for may be present already
  NotifyListener();
  return NS_OK;
}

NS_IMETHODIMP nsWEBPEncoder::CloseWithStatus(nsresult aStatus)
{
  return Close();
}

void
nsWEBPEncoder::NotifyListener()
{
  // We might call this function on multiple threads (any threads that call
  // AsyncWait and any that do encoding) so we lock to avoid notifying the
  // listener twice about the same data (which generally leads to a truncated
  // image).
  ReentrantMonitorAutoEnter autoEnter(mReentrantMonitor);

  if (mCallback &&
      (mImageBufferUsed - mImageBufferReadPoint >= mNotifyThreshold ||
       mFinished)) {
    nsCOMPtr<nsIInputStreamCallback> callback;
    if (mCallbackTarget) {
      callback = NS_NewInputStreamReadyEvent(mCallback, mCallbackTarget);
    } else {
      callback = mCallback;
    }

    NS_ASSERTION(callback, "Shouldn't fail to make the callback");
    // Null the callback first because OnInputStreamReady could reenter
    // AsyncWait
    mCallback = nullptr;
    mCallbackTarget = nullptr;
    mNotifyThreshold = 0;

    callback->OnInputStreamReady(this);
  }
}
