#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include "DecoderSurfaceAllocator.h"
#include "common/VaapiUtils.h"
#include "common/lock.h"
#include "common/log.h"
#include "common/lock.h"

#include <vector>
#include <set>
#include <deque>

using namespace YamiMediaCodec;

struct Pool {
    Lock lock;
    std::vector<intptr_t> allocated;
    std::deque<intptr_t> freed;
    std::set<intptr_t> used;
    std::vector<VASurfaceID> surfaces;
    VADisplay display;
};

struct Allocator {
    SurfaceAllocator allocator;

    VADisplay display;

    //these two variables only use to track output buffer;
    YamiMediaCodec::Lock lock;
    std::set<Pool*> pools;
};

YamiStatus getSurface(SurfaceAllocParams* thiz,intptr_t* surface)
{
    Pool* p = (Pool*)thiz->user;
    AutoLock lock(p->lock);

    if (p->freed.empty())
        return YAMI_DECODE_NO_SURFACE;
    *surface = p->freed.front();
    p->used.insert(*surface);
    p->freed.pop_front();
    return YAMI_SUCCESS;
}

YamiStatus putSurface(SurfaceAllocParams* thiz, intptr_t surface)
{
    Pool* p = (Pool*)thiz->user;
    AutoLock lock(p->lock);

    if (p->used.find(surface) == p->used.end()) {
        ERROR("put wrong surface, id = %p", (void*)surface);
        return YAMI_INVALID_PARAM;
    }
    p->used.erase(surface);
    p->freed.push_back(surface);
    return YAMI_SUCCESS;

}

static Pool* createSurfacePool(VADisplay display, SurfaceAllocParams* params)
{
    std::vector<VASurfaceID> surfaces;
    //This need match your display queue size
    static const int EXTRA_SIZE = 5;
    params->size += EXTRA_SIZE;
    surfaces.resize(params->size);
    uint32_t rtFormat = getRtFormat(params->fourcc);
    if (!rtFormat) {
        return NULL;
    }
    uint32_t vaFourcc = params->fourcc;
    VASurfaceAttrib attrib;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = vaFourcc;

    VAStatus status = vaCreateSurfaces(display, rtFormat, params->width, params->height,
            &surfaces[0], surfaces.size(),
            &attrib, 1);

    if (status != VA_STATUS_SUCCESS) {
        ERROR("create surface failed, %s", vaErrorStr(status));
        return NULL;
    }

    Pool* p = new Pool;

    p->surfaces.swap(surfaces);
    p->allocated.reserve(p->surfaces.size());
    for (size_t i = 0; i < p->surfaces.size(); i++) {
        intptr_t s = (intptr_t)p->surfaces[i];
        p->allocated.push_back(s);
        p->freed.push_back(s);
    }
    p->display = display;
    params->user = p;
    params->getSurface = getSurface;
    params->putSurface = putSurface;
    params->surfaces = &p->allocated[0];
    return p;
}

bool isFreedSurface(Pool* p, intptr_t surface)
{
     AutoLock lock(p->lock);
     for (size_t i = 0; i < p->freed.size(); i++) {
        if (p->freed[i] == surface)
            return true;
     }
     return false;
}

static YamiStatus allocSurfaces(SurfaceAllocator* allocator, SurfaceAllocParams* params)
{
    Allocator* a  = (Allocator*)allocator->user;
    Pool* p = createSurfacePool(a->display, params);
    if (!p)
        return YAMI_INVALID_PARAM;
    AutoLock lock(a->lock);
    a->pools.insert(p);

    return YAMI_SUCCESS;
}

static YamiStatus freeSurfaces(SurfaceAllocator* allocator, SurfaceAllocParams* params)
{
    Allocator* a  = (Allocator*)allocator->user;
    Pool* p = (Pool*)params->user;
    {
        vaDestroySurfaces(p->display, &p->surfaces[0], p->surfaces.size());
        AutoLock lock(a->lock);
        a->pools.erase(p);
    }
    delete p;
    return YAMI_SUCCESS;
}

static void unrefAllocator(SurfaceAllocator* allocator)
{
    Allocator* a = (Allocator*)allocator;
    if (a->pools.size()) {
        ERROR("we have unreleased pool");
    }
    ERROR("+unrefAllocator");
    delete a;
}

SurfaceAllocator* createDecoderSurfaceAllocator(const NativeDisplay* display)
{
    if (display->type != NATIVE_DISPLAY_VA) {
        ERROR("Demo allocator only supports NATIVE_DISPLAY_VA, we need use va to create Surface");
        return NULL;
    }
    Allocator* a = new Allocator;
    if (!a)
        return NULL;

    a->allocator.alloc = allocSurfaces;
    a->allocator.free  = freeSurfaces;
    a->allocator.unref = unrefAllocator;

    a->display = (VADisplay)display->handle;
    a->allocator.user = a;

    ERROR("+createDecoderSurfaceAllocator");
    return &a->allocator;
}

void checkDecoderOutput(SurfaceAllocator* allocator, intptr_t surface)
{
    Allocator* a = (Allocator*)allocator->user;
    AutoLock lock(a->lock);
    for (std::set<Pool*>::iterator it = a->pools.begin();
        it != a->pools.end();
        it ++) {
        if (isFreedSurface((*it), surface)) {
            ERROR("output a free surface");
        }

     }
}

