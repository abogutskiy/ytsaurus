﻿#include "stdafx.h"

#include "reader_thread.h"

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

TLazyPtr<TActionQueue> ReaderThread(TActionQueue::CreateFactory("ChunkrReader"));

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
