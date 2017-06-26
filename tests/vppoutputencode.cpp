/*
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "vppoutputencode.h"
#include <Yami.h>

EncodeParamsVP9::EncodeParamsVP9()
    : referenceMode(0)
{ /* do nothig*/
}

EncodeParams::EncodeParams()
    : rcMode(RATE_CONTROL_CQP)
    , initQp(26)
    , bitRate(0)
    , fps(30)
    , ipPeriod(1)
    , intraPeriod(30)
    , numRefFrames(1)
    , idrInterval(0)
    , codec("")
    , enableCabac(true)
    , enableDct8x8(false)
    , enableDeblockFilter(true)
    , deblockAlphaOffsetDiv2(2)
    , deblockBetaOffsetDiv2(2)
    , diffQPIP(0)
    , diffQPIB(0)
    , temporalLayerNum(1)
    , priorityId(0)
    , enableLowPower(false)
    , targetPercentage(95)
    , windowSize(1000)
    , initBufferFullness(0)
    , bufferSize(0)
    , qualityLevel(VIDEO_PARAMS_QUALITYLEVEL_NONE)
{
    memset(layerBitRate, 0, sizeof(layerBitRate));
}

TranscodeParams::TranscodeParams()
    : m_encParams()
    , frameCount(UINT_MAX)
    , iWidth(0)
    , iHeight(0)
    , oWidth(0)
    , oHeight(0)
    , fourcc(0)
{
    /*nothing to do*/
}

bool VppOutputEncode::init(const char* outputFileName, uint32_t fourcc,
    int width, int height, const char* codecName, int fps)
{
    if(!width || !height)
        if (!guessResolution(outputFileName, width, height))
            return false;
    m_fourcc = fourcc != YAMI_FOURCC_P010 ? YAMI_FOURCC_NV12 : YAMI_FOURCC_P010;
    m_width = width;
    m_height = height;
    m_output.reset(EncodeOutput::create(outputFileName, m_width, m_height, fps, codecName));
    return bool(m_output);
}

void VppOutputEncode::initOuputBuffer()
{
    uint32_t maxOutSize;
    m_encoder->getMaxOutSize(&maxOutSize);
    m_buffer.resize(maxOutSize);
    m_outputBuffer.bufferSize = maxOutSize;
    m_outputBuffer.format = OUTPUT_EVERYTHING;
    m_outputBuffer.data = &m_buffer[0];

}

static void setEncodeParam(const SharedPtr<IVideoEncoder>& encoder,
    int width, int height, const EncodeParams* encParam,
    const char* mimeType, uint32_t fourcc)
{
    //configure encoding parameters
    VideoParamsCommon encVideoParams;
    encVideoParams.size = sizeof(VideoParamsCommon);
    encoder->getParameters(VideoParamsTypeCommon, &encVideoParams);
    encVideoParams.resolution.width = width;
    encVideoParams.resolution.height = height;

    //frame rate parameters.
    encVideoParams.frameRate.frameRateDenom = 1;
    encVideoParams.frameRate.frameRateNum = encParam->fps;

    //picture type and bitrate
    encVideoParams.intraPeriod = encParam->intraPeriod;
    encVideoParams.ipPeriod = encParam->ipPeriod;
    encVideoParams.rcParams.bitRate = encParam->bitRate;
    encVideoParams.rcParams.initQP = encParam->initQp;
    encVideoParams.rcParams.diffQPIP = encParam->diffQPIP;
    encVideoParams.rcParams.diffQPIB = encParam->diffQPIB;
    encVideoParams.rcMode = encParam->rcMode;

    encVideoParams.numRefFrames = encParam->numRefFrames;
    encVideoParams.enableLowPower = encParam->enableLowPower;
    if (YAMI_FOURCC_P010 == fourcc)
        encVideoParams.bitDepth = 10;
    else
        encVideoParams.bitDepth = 8;

    if (VA_RC_CQP == encVideoParams.rcMode) //for CQP mode
        encVideoParams.temporalLayers.length = encParam->temporalLayerNum - 1;
    else { //for CBR or VBR mode
        uint32_t i = 0;
        for (i = 0; i < SVCT_RATE_BUFFER_LENGTH; i++) {
            if (!encParam->layerBitRate[i])
                break;
            encVideoParams.temporalLayers.bitRate[i] = encParam->layerBitRate[i];
        }
        encVideoParams.temporalLayers.length = i;
    }
    encVideoParams.size = sizeof(VideoParamsCommon);
    encoder->setParameters(VideoParamsTypeCommon, &encVideoParams);

    VideoParamsHRD encVideoParamsHRD;
    encVideoParamsHRD.size = sizeof(VideoParamsHRD);
    encoder->getParameters(VideoParamsTypeHRD, &encVideoParamsHRD);
    encVideoParamsHRD.targetPercentage = encParam->targetPercentage;
    encVideoParamsHRD.windowSize = encParam->windowSize;
    encVideoParamsHRD.initBufferFullness = encParam->initBufferFullness;
    encVideoParamsHRD.bufferSize = encParam->bufferSize;
    encVideoParamsHRD.size = sizeof(VideoParamsHRD);
    encoder->setParameters(VideoParamsTypeHRD, &encVideoParamsHRD);

    if (encParam->qualityLevel != VIDEO_PARAMS_QUALITYLEVEL_NONE) {
        VideoParamsQualityLevel encVideoParamsQualityLevel;
        encVideoParamsQualityLevel.size = sizeof(VideoParamsQualityLevel);
        encoder->getParameters(VideoParamsTypeQualityLevel, &encVideoParamsQualityLevel);
        encVideoParamsQualityLevel.level = encParam->qualityLevel;
        encVideoParamsQualityLevel.size = sizeof(VideoParamsQualityLevel);
        encoder->setParameters(VideoParamsTypeQualityLevel, &encVideoParamsQualityLevel);
    }

    // configure AVC encoding parameters
    VideoParamsAVC encVideoParamsAVC;
    if (!strcmp(mimeType, YAMI_MIME_H264)) {
        encVideoParamsAVC.size = sizeof(VideoParamsAVC);
        encoder->getParameters(VideoParamsTypeAVC, &encVideoParamsAVC);
        encVideoParamsAVC.idrInterval = encParam->idrInterval;
        encVideoParamsAVC.size = sizeof(VideoParamsAVC);
#if YAMI_CHECK_API_VERSION(0, 2, 1)
        encVideoParamsAVC.enableCabac = encParam->enableCabac;
        encVideoParamsAVC.enableDct8x8 = encParam->enableDct8x8;
        encVideoParamsAVC.enableDeblockFilter = encParam->enableDeblockFilter;
        encVideoParamsAVC.deblockAlphaOffsetDiv2
            = encParam->deblockAlphaOffsetDiv2;
        encVideoParamsAVC.deblockBetaOffsetDiv2
            = encParam->deblockBetaOffsetDiv2;
#else
        ERROR("version num of YamiAPI should be greater than or enqual to %s, "
              "\n%s ",
              "0.2.1", "or enableCabac, enableDct8x8 and enableDeblockFilter "
                       "will use the default value");
#endif
        encVideoParamsAVC.priorityId = encParam->priorityId;

        encoder->setParameters(VideoParamsTypeAVC, &encVideoParamsAVC);

        VideoConfigAVCStreamFormat streamFormat;
        streamFormat.size = sizeof(VideoConfigAVCStreamFormat);
        streamFormat.streamFormat = AVC_STREAM_FORMAT_ANNEXB;
        encoder->setParameters(VideoConfigTypeAVCStreamFormat, &streamFormat);
    }

    // configure VP9 encoding parameters
    VideoParamsVP9 encVideoParamsVP9;
    if (!strcmp(mimeType, YAMI_MIME_VP9)) {
      encoder->getParameters(VideoParamsTypeVP9, &encVideoParamsVP9);
         encVideoParamsVP9.referenceMode = encParam->m_encParamsVP9.referenceMode;
         encoder->setParameters(VideoParamsTypeVP9, &encVideoParamsVP9);
    }
}

bool VppOutputEncode::config(NativeDisplay& nativeDisplay, const EncodeParams* encParam)
{
    m_encoder.reset(createVideoEncoder(m_output->getMimeType()), releaseVideoEncoder);
    if (!m_encoder)
        return false;
    m_encoder->setNativeDisplay(&nativeDisplay);
    m_mime = m_output->getMimeType();
    setEncodeParam(m_encoder, m_width, m_height, encParam, m_mime, m_fourcc);

    Encode_Status status = m_encoder->start();
    assert(status == ENCODE_SUCCESS);
    initOuputBuffer();
    return true;
}

bool VppOutputEncode::output(const SharedPtr<VideoFrame>& frame)
{
    Encode_Status status = ENCODE_SUCCESS;
    bool drain = !frame;
    if (frame) {

        status = m_encoder->encode(frame);
        if (status != ENCODE_SUCCESS) {
            fprintf(stderr, "encode failed status = %d\n", status);
            return false;
        }
    }
    else {
        m_encoder->flush();
    }
    do {
        status = m_encoder->getOutput(&m_outputBuffer, drain);
        if (status == ENCODE_SUCCESS
            && !m_output->write(m_outputBuffer.data, m_outputBuffer.dataSize))
             assert(0);

        if (status == ENCODE_BUFFER_TOO_SMALL) {
            m_outputBuffer.bufferSize = (m_outputBuffer.bufferSize * 3) / 2;
            m_buffer.resize(m_outputBuffer.bufferSize);
            m_outputBuffer.data = &m_buffer[0];
        }

    } while (status != ENCODE_BUFFER_NO_MORE);
    return true;

}
