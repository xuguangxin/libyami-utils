#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include "DecoderSurfaceAllocator.h"
#include "common/VaapiUtils.h"
#include "common/lock.h"

#include <deque>


using namespace YamiMediaCodec;
YamiStatus getSurface(SurfaceAllocParams* thiz, intptr_t* surface);
YamiStatus putSurface(SurfaceAllocParams* thiz, intptr_t surface);

class DecoderSurfacePool
{
public:
    static DecoderSurfacePool* create(VADisplay display, SurfaceAllocParams* params)
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
        return new DecoderSurfacePool(display, surfaces, params);

    }
    ~DecoderSurfacePool()
    {
        vaDestroySurfaces(m_display, &m_surfaces[0], m_surfaces.size());
    }
    YamiStatus onGetSurface(intptr_t* surface)
    {
        AutoLock lock(m_lock);

        if (m_freed.empty())
            return YAMI_DECODE_NO_SURFACE;
        *surface = m_freed.front();
        m_used.insert(*surface);
        m_freed.pop_front();
        return YAMI_SUCCESS;

    }

    YamiStatus onPutSurface(intptr_t surface)
    {
        AutoLock lock(m_lock);

        if (m_used.find(surface) == m_used.end()) {
            ERROR("put wrong surface, id = %p", (void*)surface);
            return YAMI_INVALID_PARAM;
        }
        m_used.erase(surface);
        m_freed.push_back(surface);
        return YAMI_SUCCESS;

    }
    bool isFreedSurface(intptr_t surface)
    {
         AutoLock lock(m_lock);
         for (size_t i = 0; i < m_freed.size(); i++) {
            if (m_freed[i] == surface)
                return true;
         }
         return false;
    }
private:
    DecoderSurfacePool(VADisplay display, std::vector<VASurfaceID>& surfaces, SurfaceAllocParams* params)
    {
        m_surfaces.swap(surfaces);
        m_allocated.reserve(m_surfaces.size());
        for (size_t i = 0; i < m_surfaces.size(); i++) {
            intptr_t s = (intptr_t)m_surfaces[i];
            m_allocated.push_back(s);
            m_freed.push_back(s);
        }
        m_display = display;
        params->user = this;
        params->getSurface = getSurface;
        params->putSurface = putSurface;
        params->surfaces = &m_allocated[0];
    }

    Lock m_lock;
    std::vector<intptr_t> m_allocated;
    std::deque<intptr_t> m_freed;
    std::set<intptr_t> m_used;
    std::vector<VASurfaceID> m_surfaces;
    VADisplay m_display;
};

YamiStatus getSurface(SurfaceAllocParams* thiz, intptr_t* surface)
{
    DecoderSurfacePool* p = (DecoderSurfacePool*)thiz->user;
    return p->onGetSurface(surface);
}

YamiStatus putSurface(SurfaceAllocParams* thiz, intptr_t surface)
{
    DecoderSurfacePool* p = (DecoderSurfacePool*)thiz->user;
    return p->onPutSurface(surface);
}

static YamiStatus allocSurfaces(SurfaceAllocator* thiz, SurfaceAllocParams* params)
{
    DecoderSurfaceAllocator* a  = (DecoderSurfaceAllocator*)thiz;
    return a->onAlloc(params);
}

static YamiStatus freeSurfaces(SurfaceAllocator* thiz, SurfaceAllocParams* params)
{
    DecoderSurfaceAllocator* a = (DecoderSurfaceAllocator*)thiz;
    return a->onFree(params);
}

static void unrefAllocator(SurfaceAllocator* thiz)
{
    DecoderSurfaceAllocator* a = (DecoderSurfaceAllocator*)thiz;
    return a->onUnref();
}

DecoderSurfaceAllocator::DecoderSurfaceAllocator(NativeDisplay& display)
{
    alloc = allocSurfaces;
    free  = freeSurfaces;
    unref = unrefAllocator;
    ASSERT(display.type == NATIVE_DISPLAY_VA);
    m_display = (VADisplay)display.handle;
    ERROR("+DecoderSurfaceAllocator");
}

YamiStatus DecoderSurfaceAllocator::onAlloc(SurfaceAllocParams* params)
{
    DecoderSurfacePool* p = DecoderSurfacePool::create(m_display, params);
    if (!p)
        return YAMI_INVALID_PARAM;
    AutoLock lock(m_lock);
    m_pools.insert(p);

    return YAMI_SUCCESS;
}
YamiStatus DecoderSurfaceAllocator::onFree(SurfaceAllocParams* params)
{
    DecoderSurfacePool* p = (DecoderSurfacePool*)params->user;
    {
        AutoLock lock(m_lock);
        m_pools.erase(p);
    }
    delete p;
    return YAMI_SUCCESS;
}
void DecoderSurfaceAllocator::onUnref()
{
    /* do nothing, the SharedPtr will release us */
}

DecoderSurfaceAllocator::~DecoderSurfaceAllocator()
{
    if (m_pools.size()) {
        ERROR("we have released pool");
    }
    ERROR("~DecoderSurfaceAllocator");
}

void DecoderSurfaceAllocator::checkOutput(intptr_t surface)
{
    AutoLock lock(m_lock);
    for (std::set<DecoderSurfacePool*>::iterator it = m_pools.begin();
        it != m_pools.end();
        it ++) {
        if ((*it)->isFreedSurface(surface)) {
            ERROR("output a free surface");
        }

     }
}
