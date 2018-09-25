/*
 * Copyright (C) 2013-2014 Intel Corporation. All rights reserved.
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

#include "vppinputoutput.h"
#include "decodeinput.h"
#include "common/basesurfaceallocator.h"
#include <Yami.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
#include <android/native_window.h>
#include <system/window.h>
#include <ui/GraphicBufferMapper.h>
#include <hardware/hardware.h>
#include <hardware/gralloc1.h>
#include <va/va_android.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <map>
#include <vector>

using namespace YamiMediaCodec;
using namespace android;
using namespace std;

#ifndef CHECK_EQ
#define CHECK_EQ(a, b) do {                     \
            if ((a) != (b)) {                   \
                assert(0 && "assert fails");    \
            }                                   \
    } while (0)
#endif

#define ANDROID_DISPLAY 0x18C34078

const int HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL = 0x100;

//this should below to https://github.com/intel/minigbm/blob/master/cros_gralloc/i915_private_android_types.h
//we can't find a good way to include it, so we write it down here
typedef int32_t (*GRALLOC1_PFN_SET_INTERLACE)(gralloc1_device_t *device, buffer_handle_t buffer, uint32_t interlace);
#define GRALLOC1_FUNCTION_SET_INTERLACE 104

typedef int32_t /*gralloc1_error_t*/ (*GRALLOC1_PFN_GET_PRIME)(
        gralloc1_device_t *device, buffer_handle_t buffer, uint32_t *prime);
#define GRALLOC1_FUNCTION_GET_PRIME 103

typedef int32_t /*gralloc1_error_t*/ (*GRALLOC1_PFN_GET_BYTE_STRIDE)(
        gralloc1_device_t *device, buffer_handle_t buffer, uint32_t *outStride, uint32_t size);
#define GRALLOC1_FUNCTION_GET_BYTE_STRIDE 102

class Gralloc1
{
public:
    static SharedPtr<Gralloc1> create()
    {
        SharedPtr<Gralloc1> g(new Gralloc1);
        if (!g->init())
            g.reset();
        return g;
    }

    bool getByteStride(buffer_handle_t handle, uint32_t *outStride, uint32_t size)
    {
        return m_getByteStride(m_device, handle, outStride, size) == 0;
    }

    bool getPrime(buffer_handle_t handle, uint32_t* prime)
    {
        return m_getPrime(m_device, handle, prime) == 0;
    }

    bool setInterlace(buffer_handle_t handle, bool interlace)
    {
        uint32_t i = interlace;
        return m_setInterlace(m_device, handle, i) == 0;
    }

    ~Gralloc1()
    {
        if (m_device)
             gralloc1_close(m_device);
        //how to close module?

    }

private:
    bool init()
    {
        CHECK_EQ(0,  hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &m_module));
        CHECK_EQ(0, gralloc1_open(m_module, &m_device));
        m_setInterlace = (GRALLOC1_PFN_SET_INTERLACE)m_device->getFunction(m_device, GRALLOC1_FUNCTION_SET_INTERLACE);
        m_getPrime = (GRALLOC1_PFN_GET_PRIME)m_device->getFunction(m_device, GRALLOC1_FUNCTION_GET_PRIME);
        m_getByteStride = (GRALLOC1_PFN_GET_BYTE_STRIDE)m_device->getFunction(m_device, GRALLOC1_FUNCTION_GET_BYTE_STRIDE);
        return m_setInterlace && m_getPrime && m_getByteStride;
    }
    const struct hw_module_t* m_module = NULL;
    gralloc1_device_t* m_device = NULL;
    GRALLOC1_PFN_SET_INTERLACE m_setInterlace = NULL;
    GRALLOC1_PFN_GET_PRIME m_getPrime = NULL;
    GRALLOC1_PFN_GET_BYTE_STRIDE  m_getByteStride = NULL;
};

YamiStatus getSurface(SurfaceAllocParams* param, intptr_t* surface);

YamiStatus putSurface(SurfaceAllocParams* param, intptr_t surface);

class SurfacePool
{
    static const intptr_t INVALID_ID = (intptr_t)VA_INVALID_SURFACE;
    intptr_t getSurfaceId(ANativeWindowBuffer* buffer)
    {
        Buffer2SurfaceMap::iterator it = m_buffer2surface.find(buffer);
        if (it != m_buffer2surface.end())
            return it->second;
        return INVALID_ID;
    }

    void setGeometry(const SurfaceAllocParams* params)
    {
        sp<ANativeWindow> win = m_surface;
        CHECK_EQ(0, native_window_set_buffers_dimensions(
                    win.get(),
                    params->width,
                    params->height));
        CHECK_EQ(0, native_window_set_buffers_format(
                    win.get(),
                    HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL));

    }
    void setBufferSize(SurfaceAllocParams* params)
    {
        sp<ANativeWindow> win = m_surface;

        int min = 0;
        CHECK_EQ(0, win->query(
            win.get(), NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
            &min));
        int count = min + params->size + 3; //+3 for extra
        CHECK_EQ(0, native_window_set_buffer_count(win.get(), count));

        params->size = count;
    }

    void setCallbaks(SurfaceAllocParams* params)
    {
        params->user = this;
        params->getSurface = ::getSurface;
        params->putSurface = ::putSurface;
    }

    YamiStatus createSurfaces(SurfaceAllocParams* params)
    {
        sp<ANativeWindow> win = m_surface;
        vector<ANativeWindowBuffer*> buffers;
        buffers.reserve(params->size);

        //dequeue all buffers
        for (uint32_t i = 0; i < params->size; i++) {
            ANativeWindowBuffer* buf;
            CHECK_EQ(0, native_window_dequeue_buffer_and_wait(win.get(), &buf));
            intptr_t id = createVaSurface(buf);
            CHECK_EQ(true, id != INVALID_ID);
            m_surfac2buffer[id] = buf;
            m_buffer2surface[buf] = id;
            m_surfaces.push_back(id);
        }
        params->surfaces = &m_surfaces[0];

        //cancel all buffers;
        for (uint32_t i = 0; i < params->size; i++) {
            CHECK_EQ(0, win->cancelBuffer(win.get(), buffers[i], -1));
        }
        return YAMI_SUCCESS;
    }

    intptr_t createVaSurface(const ANativeWindowBuffer* buf)
    {
        uint32_t pitch[2];
        if (!m_gralloc->getByteStride(buf->handle, pitch, 2))
            return INVALID_ID;

        VASurfaceAttrib attrib;
        memset(&attrib, 0, sizeof(attrib));

        VASurfaceAttribExternalBuffers external;
        memset(&external, 0, sizeof(external));

        external.pixel_format = VA_FOURCC_NV12;
        external.width = buf->width;
        external.height = buf->height;
        external.pitches[0] = pitch[0];
        external.pitches[1] = pitch[1];
        external.offsets[0] = 0;
        external.offsets[1] = pitch[0] * buf->height;
        external.num_planes = 2;
        external.num_buffers = 1;
        uint32_t handle;
        if (!m_gralloc->getPrime(buf->handle, &handle)) {
            ERROR("get prime failed");
            return INVALID_ID;
        }
        external.buffers = (long unsigned int*)&handle; //graphic handel
        external.flags = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

        attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attrib.type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
        attrib.value.type = VAGenericValueTypePointer;
        attrib.value.value.p = &external;

        VASurfaceID id;
        VAStatus vaStatus = vaCreateSurfaces(m_vaDisplay, VA_RT_FORMAT_YUV420,
                            buf->width, buf->height, &id, 1, &attrib, 1);
        if (vaStatus != VA_STATUS_SUCCESS)
            return INVALID_ID;

        return (intptr_t)id;
    }

public:
    SurfacePool(sp<Surface>& surface, VADisplay display, SharedPtr<Gralloc1>& gralloc)
        : m_surface(surface)
        , m_vaDisplay(display)
        , m_gralloc(gralloc)
    {
    }
    YamiStatus doAlloc(SurfaceAllocParams* params)
    {
        setGeometry(params);
        setBufferSize(params);
        setCallbaks(params);
        return createSurfaces(params);
    }

    YamiStatus doFree(SurfaceAllocParams* params)
    {
        for (uint32_t i = 0; i < params->size; i++) {
            VASurfaceID id = params->surfaces[i];
            vaDestroySurfaces(m_vaDisplay, &id, 1);
        }
        return YAMI_SUCCESS;
    }

    YamiStatus getSurface(intptr_t* surface)
    {
        sp<ANativeWindow> win = m_surface;
        ANativeWindowBuffer* buf;
        CHECK_EQ(0, native_window_dequeue_buffer_and_wait(win.get(), &buf));
        *surface = getSurfaceId(buf);
        return YAMI_SUCCESS;
    }

    YamiStatus putSurface(intptr_t surface)
    {
        /* nothing*/
        return YAMI_SUCCESS;
    }

    ANativeWindowBuffer* getNativeWindowBuffer(intptr_t id)
    {
        Surface2BufferMap::iterator it = m_surfac2buffer.find(id);
        if (it != m_surfac2buffer.end())
            return it->second;
        return NULL;
    }

private:

    typedef std::map<intptr_t, ANativeWindowBuffer*> Surface2BufferMap;
    typedef std::map<ANativeWindowBuffer*, intptr_t> Buffer2SurfaceMap;
    Surface2BufferMap m_surfac2buffer;
    Buffer2SurfaceMap m_buffer2surface;
    vector<intptr_t> m_surfaces;
    sp<Surface> m_surface;
    VADisplay m_vaDisplay;
    SharedPtr<Gralloc1> m_gralloc;
};

YamiStatus getSurface(SurfaceAllocParams* param, intptr_t* surface)
{
    SurfacePool* p = (SurfacePool*)param->user;
    return p->getSurface(surface);
}

YamiStatus putSurface(SurfaceAllocParams* param, intptr_t surface)
{
    SurfacePool* p = (SurfacePool*)param->user;
    return p->putSurface(surface);
}

class AndroidAllocator : public BaseSurfaceAllocator
{
public:
    static SharedPtr<AndroidAllocator> create(sp<Surface>& surface, VADisplay dispaly)
    {
        SharedPtr<AndroidAllocator> alloc(new AndroidAllocator(surface, dispaly));
        if (!alloc->init())
            alloc.reset();
        return alloc;
    }

    ANativeWindowBuffer* getNativeWindowBuffer(SharedPtr<VideoFrame>& frame)
    {
        intptr_t id = frame->surface;
        Surface2Pool::iterator it = m_surface2Pool.find(id);
        if (it == m_surface2Pool.end())
            return NULL;
        SharedPtr<SurfacePool>& pool = it->second;
        return pool->getNativeWindowBuffer(id);
    }

protected:
    YamiStatus doAlloc(SurfaceAllocParams* params)
    {
        SharedPtr<SurfacePool> pool(new SurfacePool(m_surface, m_vaDisplay, m_gralloc));
        YamiStatus status = pool->doAlloc(params);
        CHECK_EQ(YAMI_SUCCESS, status);
        for (uint32_t i = 0; i < params->size; i++) {
            intptr_t id = params->surfaces[i];
            m_surface2Pool[id] = pool;
        }
        return status;
    }
    YamiStatus doFree(SurfaceAllocParams* params)
    {
        SurfacePool* pool = (SurfacePool*)params->user;
        YamiStatus status = pool->doFree(params);
        CHECK_EQ(YAMI_SUCCESS, status);
        for (uint32_t i = 0; i < params->size; i++) {
            intptr_t id = params->surfaces[i];
            m_surface2Pool.erase(id);
        }
        return status;

    }
    void doUnref()
    {
        //do nothing;
    }
private:
    AndroidAllocator(sp<Surface>& surface, VADisplay display)
        : m_surface(surface)
        , m_vaDisplay(display)
    {
    }
    bool init()
    {
        m_gralloc = Gralloc1::create();
        if (!m_gralloc.get())
            return false;
        return true;

    }
    sp<Surface> m_surface;
    VADisplay m_vaDisplay;
    SharedPtr<Gralloc1> m_gralloc;
    typedef map<intptr_t, SharedPtr<SurfacePool> > Surface2Pool;
    Surface2Pool m_surface2Pool;
};

class AndroidRenderer
{
public:
    bool render(SharedPtr<VideoFrame>& frame)
    {
        status_t err;
        sp<ANativeWindow> win = m_surface;
        ANativeWindowBuffer* buf = m_allocator->getNativeWindowBuffer(frame);
        if (!buf) {
            fprintf(stderr, "failed to get window buffer for %x\n", frame->surface);
            return false;
        }

        if (win->queueBuffer(win.get(), buf, -1) != 0) {
            fprintf(stderr, "queue buffer to native window failed\n");
            return false;
        }
        return true;
    }
    static SharedPtr<AndroidRenderer> create(VADisplay display)
    {
        SharedPtr<AndroidRenderer> r(new AndroidRenderer(display));
        if (!r->initWindow() || !r->initAllocator() || !r->initVpp()) {
            fprintf(stderr, "failed to create render \n");
            r.reset();
        }
        return r;
    }

private:
    AndroidRenderer(VADisplay display)
        : m_vaDisplay(display)
    {
    }
    bool initWindow()
    {
        static sp<SurfaceComposerClient> client = new SurfaceComposerClient();
        //create surface
        static sp<SurfaceControl> surfaceCtl = client->createSurface(String8("testsurface"), 800, 600, HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL, 0);

        // configure surface
        SurfaceComposerClient::Transaction{}
             .setLayer(surfaceCtl, 100000)
             .setPosition(surfaceCtl, 100, 100)
             .setSize(surfaceCtl, 1920, 1080)
             .apply();

        m_surface = surfaceCtl->getSurface();

        static sp<ANativeWindow> mNativeWindow = m_surface;
        int bufWidth = 1920;
        int bufHeight = 1088;

        int consumerUsage = 0;
        CHECK_EQ(NO_ERROR, mNativeWindow->query(mNativeWindow.get(), NATIVE_WINDOW_CONSUMER_USAGE_BITS, &consumerUsage));
        CHECK_EQ(0,
                 native_window_set_usage(
                 mNativeWindow.get(),
                 consumerUsage));

        CHECK_EQ(0,
                 native_window_set_scaling_mode(
                 mNativeWindow.get(),
                 NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW));


        CHECK_EQ(0, native_window_api_connect(mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA));

        return true;
    }

    bool initAllocator()
    {
        m_allocator = AndroidAllocator::create(m_surface, m_vaDisplay);
        return m_allocator.get();
    }

    //let us keep this for debug usage
    bool initVpp()
    {
        NativeDisplay nativeDisplay;
        nativeDisplay.type = NATIVE_DISPLAY_VA;
        nativeDisplay.handle = (intptr_t)m_vaDisplay;
        m_vpp.reset(createVideoPostProcess(YAMI_VPP_SCALER), releaseVideoPostProcess);
        return m_vpp->setNativeDisplay(nativeDisplay) == YAMI_SUCCESS;
    }


    sp<Surface> m_surface;
    SharedPtr<IVideoPostProcess> m_vpp;
    VADisplay m_vaDisplay;
    SharedPtr<AndroidAllocator> m_allocator;

};

class AndroidPlayer
{
public:
    bool init(int argc, char** argv)
    {
        if (argc != 2) {
            printf("usage: androidplayer xxx.264\n");
            return false;
        }

        m_input.reset(DecodeInput::create(argv[1]));
        if (!m_input) {
            fprintf(stderr, "failed to open %s", argv[1]);
            return false;
        }
        if (!initVaDisplay()) {
            fprintf(stderr, "failed to init va display");
            return false;

        }

        //init decoder
        m_decoder.reset(createVideoDecoder(m_input->getMimeType()), releaseVideoDecoder);
        if (!m_decoder) {
            fprintf(stderr, "failed create decoder for %s", m_input->getMimeType());
            return false;
        }

        //set native display
        m_decoder->setNativeDisplay(m_nativeDisplay.get());

        if(!initRenderer()) {
            fprintf(stderr, "failed to create android surface\n");
            return false;
        }


        return true;
    }

    bool run()
    {
        VideoConfigBuffer configBuffer;
        memset(&configBuffer, 0, sizeof(configBuffer));
        configBuffer.profile = VAProfileNone;
        const string codecData = m_input->getCodecData();
        if (codecData.size()) {
            configBuffer.data = (uint8_t*)codecData.data();
            configBuffer.size = codecData.size();
        }

        Decode_Status status = m_decoder->start(&configBuffer);
        assert(status == DECODE_SUCCESS);

        VideoDecodeBuffer inputBuffer;
        while (m_input->getNextDecodeUnit(inputBuffer)) {
            status = m_decoder->decode(&inputBuffer);
            if (DECODE_FORMAT_CHANGE == status) {
                //drain old buffers
                renderOutputs();
                const VideoFormatInfo *formatInfo = m_decoder->getFormatInfo();
                //resend the buffer
                status = m_decoder->decode(&inputBuffer);
            }
            if(status == DECODE_SUCCESS) {
                renderOutputs();
            } else {
                fprintf(stderr, "decode error %d\n", status);
                break;
            }
        }
        //renderOutputs();
        m_decoder->stop();
        return true;
    }

    AndroidPlayer() : m_width(0), m_height(0)
    {
    }

    ~AndroidPlayer()
    {
    }
private:
    VADisplay m_vaDisplay;

    bool initRenderer()
    {
        m_renderer = AndroidRenderer::create(m_vaDisplay);
        return m_renderer.get();
    }

    bool initVaDisplay()
    {
        unsigned int display = ANDROID_DISPLAY;
        m_vaDisplay = vaGetDisplay(&display);

        int major, minor;
        VAStatus status;
        status = vaInitialize(m_vaDisplay, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "init vaDisplay failed\n");
            return false;
        }

        m_nativeDisplay.reset(new NativeDisplay);
        m_nativeDisplay->type = NATIVE_DISPLAY_VA;
        m_nativeDisplay->handle = (intptr_t)m_vaDisplay;
        return true;
    }



    void renderOutputs()
    {
        SharedPtr<VideoFrame> srcFrame;
        do {
            srcFrame = m_decoder->getOutput();
            if (!srcFrame)
                break;

            if(!m_renderer->render(srcFrame))
                break;
        } while (1);
    }

    SharedPtr<NativeDisplay> m_nativeDisplay;

    SharedPtr<DecodeInput> m_input;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<AndroidRenderer> m_renderer;
    int m_width, m_height;
};

int main(int argc, char** argv)
{
    AndroidPlayer player;
    if (!player.init(argc, argv)) {
        ERROR("init player failed with %s", argv[1]);
        return -1;
    }
    if (!player.run()){
        ERROR("run simple player failed");
        return -1;
    }
    printf("play file done\n");

    return  0;

}
