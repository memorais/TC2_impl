/* This Source Code Form is subject to the terms of the Mozilla Public
   * License, v. 2.0. If a copy of the MPL was not distributed with this
   * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
  
#include "mozilla/Attributes.h"
#include "mozilla/ReentrantMonitor.h"
  
#include "imgIEncoder.h"

#include "nsCOMPtr.h"

#define NS_WEBPENCODER_CID \
{ /* 05848c32-1722-462d-bb49-688dd1a63ee5 */			\
 	0x05848c32,						\
	0x1722,							\
	0x462d,							\
       {0xbb, 0x49, 0x68, 0x8d, 0xd1, 0xa6, 0x3e, 0xe5} 	\
}
/*
extern "C" {
#include "webp/encode.h"
}

class nsWEBPEncoder MOZ_FINAL : public imgIEncoder
{

}*/
