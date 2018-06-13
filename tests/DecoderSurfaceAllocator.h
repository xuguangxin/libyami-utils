/*
 * Copyright (C) 2014 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DecoderSurfaceAllocator_h
#define DecoderSurfaceAllocator_h

#include "common/log.h"
#include "common/lock.h"
#include <Yami.h>
#include <vector>
#include <set>

class DecoderSurfacePool;
class DecoderSurfaceAllocator : public SurfaceAllocator
{
public:
    DecoderSurfaceAllocator(NativeDisplay& display);
    ~DecoderSurfaceAllocator();

    YamiStatus onAlloc(SurfaceAllocParams* params);
    YamiStatus onFree(SurfaceAllocParams* params);
    void       onUnref();
    void checkOutput(intptr_t surface);
private:
    VADisplay m_display;

    //these two variables only use to track output buffer;
    YamiMediaCodec::Lock m_lock;
    std::set<DecoderSurfacePool*> m_pools;
};

#endif //DecoderSurfaceAllocator_h
