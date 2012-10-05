/*
 * Hardware encoding support for DM365 SoC
 * Copyright (c) 2012 Jan Pohanka
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
* @file
* Hardware encoder for DM365 SoC
*/

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "internal.h"

#include <xdc/std.h>
#include <ti/sdo/ce/CERuntime.h>
#include <ti/sdo/ce/Engine.h>
#include <ti/sdo/ce/video1/videnc1.h>
#include <ti/sdo/codecs/h264enc/ih264venc.h>
#include <ti/sdo/ce/image1/imgenc1.h>
#include <ti/sdo/codecs/jpegenc/ijpegenc.h>

typedef struct DM365Context {
    AVFrame image;
    Engine_Handle hEngine;
    VISA_Handle hEncode;
    void *codecParams;
    void *codecDynParams;
} DM365Context;

/*
 * Default parameters for hardware encoder
 */
#if CONFIG_LIBDM365_H264_ENCODER
static const VIDENC1_Params Venc1_Params_DEFAULT = {
    sizeof(VIDENC1_Params),           /* size */
    XDM_DEFAULT,                      /* encodingPreset */
    IVIDEO_LOW_DELAY,                 /* rateControlPreset */
    1200,                             /* maxHeight */
    1600,                             /* maxWidth */
    30000,                            /* maxFrameRate */
    6000000,                          /* maxBitRate */
    XDM_BYTE,                         /* dataEndianness */
    0,                                /* maxInterFrameInterval */
    XDM_YUV_420P,                     /* inputChromaFormat */
    IVIDEO_PROGRESSIVE,               /* inputContentType */
    XDM_CHROMA_NA                     /* reconChromaFormat */
};

static const VIDENC1_DynamicParams Venc1_DynamicParams_DEFAULT = {
    sizeof(IVIDENC1_DynamicParams),   /* size */
    1200,                             /* inputHeight */
    1600,                             /* inputWidth */
    30000,                            /* refFrameRate */
    30000,                            /* targetFrameRate */
    6000000,                          /* targetBitRate */
    30,                               /* intraFrameInterval */
    XDM_ENCODE_AU,                    /* generateHeader */
    0,                                /* captureWidth */
    IVIDEO_NA_FRAME,                  /* forceFrame */
    1,                                /* interFrameInterval */
    0                                 /* mbDataFlag */
};
#endif

#if CONFIG_LIBDM365_JPEG_ENCODER
static const IMGENC1_Params Ienc1_Params_DEFAULT = {
    sizeof(IMGENC1_Params),
    1200,
    1600,
    XDM_DEFAULT,
    XDM_BYTE,
    XDM_YUV_420P
};

static const IIMGENC1_DynamicParams Ienc1_DynamicParams_DEFAULT = {
    sizeof(IMGENC1_DynamicParams),
    XDM_DEFAULT,
    XDM_YUV_420P,
    0,
    0,
    0,
    XDM_ENCODE_AU,
    75
};

IJPEGENC_Params IJPEGENC_PARAMS = {
        .halfBufCB = NULL,
        .halfBufCBarg = NULL,
};

IJPEGENC_DynamicParams IJPEGENC_DYNAMICPARAMS = {
        .rstInterval = 84,
        .disableEOI = 0,
        .rotation = 0,
        .customQ = NULL,
};
#endif

#if CONFIG_LIBDM365_JPEG_ENCODER
static IMGENC1_Handle imgenc_create(AVCodecContext *avctx, Engine_Handle hEngine,
        const char *encoder, IMGENC1_Params *params, IMGENC1_DynamicParams *dynParams)
{
    IMGENC1_Handle hEncode;
    IMGENC1_Status encStatus;
    XDAS_Int32 status;

    hEncode = IMGENC1_create(hEngine, (String) encoder, params);
    if (hEncode == 0)
        return NULL;

    encStatus.size = sizeof(IMGENC1_Status);
    encStatus.data.buf = NULL;

    status = IMGENC1_control(hEncode, XDM_SETPARAMS, dynParams, &encStatus);
    if (status != VIDENC1_EOK) {
        DM365_JPEGENC_ERROR err = encStatus.extendedError & 0xff;
        av_log(avctx, AV_LOG_ERROR, "extended error: %x\n", err);
        IMGENC1_delete(hEncode);
        return NULL;
    }

    return hEncode;
}

static av_cold int jpeg_enc_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    IJPEGENC_Params *jpegParams;
    IJPEGENC_DynamicParams *jpegDynParams;
    IIMGENC1_Params *encParams;
    IIMGENC1_DynamicParams *dynParams;

    if (avctx->pix_fmt != PIX_FMT_NV12) {
        av_log(avctx, AV_LOG_INFO, "unsupported pixel format\n");
        return -1;
    }

    jpegParams = av_malloc(sizeof(IJPEGENC_Params));
    if (!jpegParams)
        return AVERROR(ENOMEM);

    jpegDynParams = av_malloc(sizeof(IJPEGENC_DynamicParams));
    if (!jpegDynParams) {
        av_free(jpegParams);
        return AVERROR(ENOMEM);
    }

    *jpegParams = IJPEGENC_PARAMS;
    *jpegDynParams = IJPEGENC_DYNAMICPARAMS;
    encParams = &(jpegParams->imgencParams);
    dynParams = &(jpegDynParams->imgencDynamicParams);

    *encParams = Ienc1_Params_DEFAULT;
    *dynParams = Ienc1_DynamicParams_DEFAULT;

    encParams->maxWidth = 1600;
    encParams->maxHeight = 1200;
    encParams->size = sizeof(IJPEGENC_Params);

    dynParams->inputWidth = avctx->width;
    dynParams->inputHeight = avctx->height;
    dynParams->captureWidth = avctx->width;
    dynParams->inputChromaFormat = XDM_YUV_420SP;
    dynParams->qValue = avctx->mpeg_quant;              //XXX: ???
    dynParams->size = sizeof(IJPEGENC_DynamicParams);

    ctx->codecParams = jpegParams;
    ctx->codecDynParams = jpegDynParams;

    ctx->hEncode = imgenc_create(avctx, ctx->hEngine, "jpegenc1",
            ctx->codecParams, ctx->codecDynParams);
    if (!ctx->hEncode) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create jpeg encoder\n");
        av_freep(&ctx->codecParams);
        av_freep(&ctx->codecDynParams);
        return -1;
    }

    return 0;
}
#else
static av_cold int jpeg_enc_init(AVCodecContext *avctx) { return -1; }
#endif

#if CONFIG_LIBDM365_H264_ENCODER
static VIDENC1_Handle encoder_create(AVCodecContext *avctx, Engine_Handle hEngine,
        const char *encoder, VIDENC1_Params *params, VIDENC1_DynamicParams *dynParams)
{
    VIDENC1_Handle hEncode;
    VIDENC1_Status encStatus;
    XDAS_Int32 status;

    hEncode = VIDENC1_create(hEngine, (String) encoder, params);
    if (hEncode == 0)
        return NULL;

    encStatus.size = sizeof(VIDENC1_Status);
    encStatus.data.buf = NULL;

    status = VIDENC1_control(hEncode, XDM_SETPARAMS, dynParams, &encStatus);
    if (status != VIDENC1_EOK) {
        IH264VENC_STATUS err = encStatus.extendedError;
        av_log(avctx, AV_LOG_ERROR, "extended error: %x\n", err);
        VIDENC1_delete(hEncode);
        return NULL;
    }

    return hEncode;
}

static av_cold int h264_enc_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    IH264VENC_Params *h264Params;
    IH264VENC_DynamicParams *h246DynParams;
    IVIDENC1_Params *encParams;
    IVIDENC1_DynamicParams *dynParams;

    if (avctx->pix_fmt != PIX_FMT_NV12) {
        av_log(avctx, AV_LOG_INFO, "unsupported pixel format\n");
        return -1;
    }

    h264Params = av_malloc(sizeof(IH264VENC_Params));
    if (!h264Params)
        return AVERROR(ENOMEM);

    h246DynParams = av_malloc(sizeof(IH264VENC_DynamicParams));
    if (!h246DynParams) {
        av_free(h264Params);
        return AVERROR(ENOMEM);
    }

    *h264Params = IH264VENC_PARAMS;
    *h246DynParams = H264VENC_TI_IH264VENC_DYNAMICPARAMS;
    encParams = &(h264Params->videncParams);
    dynParams = &(h246DynParams->videncDynamicParams);

    *encParams = Venc1_Params_DEFAULT;
    *dynParams = Venc1_DynamicParams_DEFAULT;

    encParams->encodingPreset  = XDM_HIGH_SPEED;
    encParams->inputChromaFormat = XDM_YUV_420SP;
    encParams->rateControlPreset = IVIDEO_NONE;
    encParams->maxBitRate = 200000;
    encParams->size = sizeof(IH264VENC_Params);

    dynParams->targetBitRate   = encParams->maxBitRate;
    dynParams->inputWidth      = avctx->width;
    dynParams->inputHeight     = avctx->height;
    dynParams->captureWidth    = avctx->width;
    dynParams->refFrameRate    = 30000;
    dynParams->targetFrameRate = 30000;
    dynParams->interFrameInterval = 0;
    dynParams->intraFrameInterval = avctx->gop_size;
    dynParams->size = sizeof(IH264VENC_DynamicParams);

    h264Params->enableVUIparams = 0x04;

    h246DynParams->VUI_Buffer = &H264VENC_TI_VUIPARAMBUFFER;
    h246DynParams->VUI_Buffer->numUnitsInTicks = avctx->time_base.num;
    h246DynParams->VUI_Buffer->timeScale = avctx->time_base.den * 2; /* field rate !!*/
    h246DynParams->VUI_Buffer->timingInfoPresentFlag = 1;
    h246DynParams->VUI_Buffer->fixedFrameRateFlag = 1;
    h246DynParams->enablePicTimSEI = 1;
    h246DynParams->idrFrameInterval = dynParams->intraFrameInterval;
    h246DynParams->rcQMax = avctx->qmax;
    h246DynParams->rcQMin = avctx->qmin;
    h246DynParams->aspectRatioX = avctx->sample_aspect_ratio.num ? avctx->sample_aspect_ratio.num : 1;
    h246DynParams->aspectRatioY = avctx->sample_aspect_ratio.den ? avctx->sample_aspect_ratio.den : 1;

    ctx->codecParams = h264Params;
    ctx->codecDynParams = h246DynParams;

    ctx->hEncode = encoder_create(avctx, ctx->hEngine, "h264enc",
            ctx->codecParams, ctx->codecDynParams);
    if (!ctx->hEncode) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create encoder\n");
        av_freep(&ctx->codecParams);
        av_freep(&ctx->codecDynParams);
        return -1;
    }

    /* dm365 h264 encoder does not support B-frames */
    avctx->has_b_frames = 0;

    return 0;
}
#else
static av_cold int h264_enc_init(AVCodecContext *avctx) { return -1; }
#endif

static av_cold int dm365_encode_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    int ret;

    /*
     * CERuntime_init() has to be called from main application
     * as well as CERuntime_exit(). Otherwise other dm365 codec
     * initialization or deinitialization could break everything
     */

    ctx->hEngine = Engine_open("encode", NULL, NULL);
    if (ctx->hEngine == NULL)
        return AVERROR(1);

    switch (avctx->codec_id) {
    case CODEC_ID_H264:
        ret = h264_enc_init(avctx);
        break;
    case CODEC_ID_MJPEG:
        ret = jpeg_enc_init(avctx);
        break;
    default:
        ret = -1;
        break;
    }

    if (ret < 0) {
        Engine_close(ctx->hEngine);
        return ret;
    }

    avctx->coded_frame = &ctx->image;

    return 0;
}

static av_cold int dm365_encode_close(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;

    if (avctx->codec_id == CODEC_ID_MJPEG)
        IMGENC1_delete(ctx->hEncode);
    else
        VIDENC1_delete(ctx->hEncode);
    av_free(ctx->codecDynParams);
    av_free(ctx->codecParams);
    Engine_close(ctx->hEngine);

    return 0;
}

#if CONFIG_LIBDM365_H264_ENCODER
static int dm365_videnc_process(AVCodecContext *avctx, uint8_t *buf, int buf_size, void *data)
{
    DM365Context *ctx = avctx->priv_data;
    IVIDEO1_BufDescIn inBufDesc;
    XDM_BufDesc outBufDesc;
    XDAS_Int32 outBufSizeArray[1];
    VIDENC1_InArgs inArgs;
    VIDENC1_OutArgs outArgs;
    XDAS_Int32 status;
    AVFrame *pic = (AVFrame *)data;
    IVIDENC1_DynamicParams *dynParams = (IVIDENC1_DynamicParams *) ctx->codecDynParams;
    int frameWidth;
    int frameHeight;

    /* inBufDesc.framePitch field is not used in encoder,
     * different pitch has to be specified through XDM_SETPARAMS */
    if (pic->linesize[0] != dynParams->captureWidth) {
        VIDENC1_Status encStatus;
        int tmp = dynParams->captureWidth;

        encStatus.size = sizeof(VIDENC1_Status);
        encStatus.data.buf = NULL;

        dynParams->captureWidth = pic->linesize[0];

        status = VIDENC1_control(ctx->hEncode, XDM_SETPARAMS, dynParams, &encStatus);
        if (status != VIDENC1_EOK) {
            IH264VENC_STATUS err = encStatus.extendedError;
            av_log(avctx, AV_LOG_ERROR, "extended error: %x\n", err);
            dynParams->captureWidth = tmp;
            return -1;
        }
    }

    frameWidth  = FFALIGN(avctx->width, 16);
    frameHeight = FFALIGN(avctx->height, 16);

    inBufDesc.frameWidth = frameWidth;
    inBufDesc.frameHeight = frameHeight;

    inBufDesc.bufDesc[0].bufSize = pic->linesize[0] * frameHeight;
    inBufDesc.bufDesc[1].bufSize = pic->linesize[1] * frameHeight/2;

    inBufDesc.bufDesc[0].buf = pic->data[0];
    inBufDesc.bufDesc[1].buf = pic->data[1];

    inBufDesc.numBufs = 2;

    outBufSizeArray[0] = buf_size;
    outBufDesc.numBufs = 1;
    outBufDesc.bufs = (XDAS_Int8 **) &buf;
    outBufDesc.bufSizes = outBufSizeArray;

    inArgs.size = sizeof(VIDENC1_InArgs);
    inArgs.inputID = 1;
    inArgs.topFieldFirstFlag = 1;

    outArgs.size = sizeof(VIDENC1_OutArgs);

    status = VIDENC1_process(ctx->hEncode, &inBufDesc, &outBufDesc,
            (VIDENC1_InArgs *) &inArgs, &outArgs);
    if (status != VIDENC1_EOK) {
        IH264VENC_STATUS err = outArgs.extendedError;
        av_log(avctx, AV_LOG_ERROR, "encoding error: %x\n", err);
        return -1;
    }

    av_log(avctx, AV_LOG_DEBUG, "bytes generated: %d\n", (int) outArgs.bytesGenerated);

    ctx->image.key_frame = 0;
    switch (outArgs.encodedFrameType) {
    case IVIDEO_I_FRAME:
        ctx->image.type = AV_PICTURE_TYPE_I;
        break;
    case IVIDEO_IDR_FRAME:
        ctx->image.key_frame = 1;
        ctx->image.type = AV_PICTURE_TYPE_I;
        break;
    case IVIDEO_P_FRAME:
        ctx->image.type = AV_PICTURE_TYPE_P;
        break;
    default:
        ctx->image.type = AV_PICTURE_TYPE_NONE;
        av_log(avctx, AV_LOG_WARNING, "unknown picture type\n");
        break;
    }

    return outArgs.bytesGenerated;
}
#endif

#if CONFIG_LIBDM365_JPEG_ENCODER
static int dm365_imgenc_process(AVCodecContext *avctx, uint8_t *buf,
        int buf_size, void *data)
{
    DM365Context *ctx = avctx->priv_data;
    XDM1_BufDesc inBufs;
    XDM1_BufDesc outBufs;
    IMGENC1_InArgs inArgs;
    IMGENC1_OutArgs outArgs;
    XDAS_Int32 status;
    AVFrame *pic = (AVFrame *)data;

    /* TODO: only for NV12 */
    inBufs.descs[0].buf = pic->data[0];
    inBufs.descs[1].buf = pic->data[1];
    inBufs.descs[0].bufSize = pic->linesize[0] * avctx->height;
    inBufs.descs[1].bufSize = pic->linesize[1] * avctx->height/2;
    inBufs.numBufs = 2;

    outBufs.numBufs = 1;
    outBufs.descs[0].buf = buf;
    outBufs.descs[0].bufSize = buf_size;

    inArgs.size = sizeof(IMGENC1_InArgs);
    outArgs.size = sizeof(IMGENC1_OutArgs);

    status = IMGENC1_process(ctx->hEncode, &inBufs, &outBufs, &inArgs, &outArgs);
    if (status != IMGENC1_EOK) {
        DM365_JPEGENC_ERROR err = outArgs.extendedError & 0xff;
        av_log(avctx, AV_LOG_ERROR, "encoding error: %x\n", err);
        return -1;
    }

    av_log(avctx, AV_LOG_DEBUG, "bytes generated: %d\n", (int) outArgs.bytesGenerated);

    return outArgs.bytesGenerated;
}
#endif

#if CONFIG_LIBDM365_H264_ENCODER
AVCodec ff_libdm365_h264_encoder =
{
    .name           = "libdm365_h264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .priv_data_size = sizeof(DM365Context),
    .init           = dm365_encode_init,
    .close          = dm365_encode_close,
    .encode         = dm365_videnc_process,
    .capabilities   = CODEC_CAP_EXPERIMENTAL | CODEC_CAP_DR1,
    .pix_fmts       = (const enum PixelFormat[]) {PIX_FMT_NV12, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("h.264 hardware encoder on dm365 SoC"),
};
#endif

#if CONFIG_LIBDM365_JPEG_ENCODER
AVCodec ff_libdm365_jpeg_encoder = {
    .name           = "libdm365_jpeg",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_MJPEG,
    .priv_data_size = sizeof(DM365Context),
    .init           = dm365_encode_init,
    .close          = dm365_encode_close,
    .encode         = dm365_imgenc_process,
    .capabilities   = CODEC_CAP_EXPERIMENTAL | CODEC_CAP_DR1,
    .pix_fmts       = (const enum PixelFormat[]) {PIX_FMT_NV12, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("h.264 hardware encoder on dm365 SoC"),
};
#endif
