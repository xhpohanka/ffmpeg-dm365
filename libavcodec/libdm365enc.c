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

typedef struct DM365Context {
    AVFrame image;
    Engine_Handle hEngine;
    VIDENC1_Handle hEncode;
    void *codecParams;
    void *codecDynParams;
} DM365Context;

/*
 * Default parameters for hardware encoder
 */
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

static VIDENC1_Handle encoder_create(Engine_Handle hEngine, const char *encoder,
        VIDENC1_Params *params, VIDENC1_DynamicParams *dynParams)
{
    VIDENC1_Handle hEncode;
    VIDENC1_Status encStatus;
    XDAS_Int32 status;

    hEncode = VIDENC1_create(hEngine, (String) encoder, params);
    if (hEncode == 0)
        return hEncode;

    encStatus.size = sizeof(VIDENC1_Status);
    encStatus.data.buf = NULL;

    status = VIDENC1_control(hEncode, XDM_SETPARAMS, dynParams, &encStatus);
    if (status != VIDENC1_EOK) {
        VIDENC1_delete(hEncode);
        hEncode = NULL;
        return hEncode;
    }

    return hEncode;
}

static av_cold int h264_enc_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    IH264VENC_Params *h264Params;
    IH264VENC_DynamicParams *h246DynParams;
    VIDENC1_Params *encParams;
    VIDENC1_DynamicParams *dynParams;

    if (avctx->pix_fmt != PIX_FMT_NV12) {
        av_log(avctx, AV_LOG_INFO, "unsupported pixel format\n");
        return -1;
    }

    h264Params = av_malloc(sizeof(IH264VENC_Params));
    if (!h264Params) {
        return AVERROR(ENOMEM);
    }
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

    encParams->inputChromaFormat = XDM_YUV_420SP;
    encParams->maxWidth  = avctx->width;
    encParams->maxHeight = avctx->height;
    encParams->encodingPreset  = XDM_HIGH_SPEED;
    encParams->inputChromaFormat = XDM_YUV_420SP;
    encParams->rateControlPreset = IVIDEO_NONE;
    encParams->maxBitRate = 200000;
    encParams->size = sizeof(IH264VENC_Params);

    dynParams->targetBitRate   = encParams->maxBitRate;
    dynParams->inputWidth      = avctx->width;
    dynParams->inputHeight     = avctx->height;
    dynParams->refFrameRate    = 30000;
    dynParams->targetFrameRate = 30000;
    dynParams->interFrameInterval = 0;
    dynParams->size = sizeof(IH264VENC_DynamicParams);

    h264Params->enableVUIparams = 0x04;

    h246DynParams->VUI_Buffer = &H264VENC_TI_VUIPARAMBUFFER;
    h246DynParams->VUI_Buffer->numUnitsInTicks = avctx->time_base.num;
    h246DynParams->VUI_Buffer->timeScale = avctx->time_base.den;
    h246DynParams->VUI_Buffer->timingInfoPresentFlag = 1;
    h246DynParams->VUI_Buffer->fixedFrameRateFlag = 1;
    h246DynParams->enablePicTimSEI = 1;

    ctx->codecParams = h264Params;
    ctx->codecDynParams = h246DynParams;

    ctx->hEncode = encoder_create(ctx->hEngine, "h264enc",
            ctx->codecParams, ctx->codecDynParams);
    if (!ctx->hEncode) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create encoder\n");
        av_freep(&ctx->codecParams);
        av_freep(&ctx->codecDynParams);
        return -1;
    }

    return 0;
}

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

    VIDENC1_delete(ctx->hEncode);
    av_free(ctx->codecDynParams);
    av_free(ctx->codecParams);
    Engine_close(ctx->hEngine);

    return 0;
}

static int encoder_process(VIDENC1_Handle hEncode,
        AVCodecContext *avctx, uint8_t *buf,
        int buf_size, void *data)
{
    DM365Context *ctx = avctx->priv_data;
    IVIDEO1_BufDescIn inBufDesc;
    XDM_BufDesc outBufDesc;
    XDAS_Int8 *inPtr;
    XDAS_Int32 outBufSizeArray[1];
    VIDENC1_InArgs inArgs;
    IH264VENC_InArgs inH264Args;
    VIDENC1_OutArgs outArgs;
    XDAS_Int32 status;
    AVFrame *pic = (AVFrame *)data;

    inBufDesc.frameWidth = avctx->width;
    inBufDesc.frameHeight = avctx->height;
    inBufDesc.framePitch = avctx->width;

    inBufDesc.bufDesc[0].bufSize = avctx->width * avctx->height;
    inBufDesc.bufDesc[1].bufSize = inBufDesc.bufDesc[0].bufSize /2;

    inPtr = pic->data[0];
    inBufDesc.bufDesc[0].buf = inPtr;
    inBufDesc.bufDesc[1].buf = inPtr + inBufDesc.bufDesc[0].bufSize;

    inBufDesc.numBufs = 2;

    outBufSizeArray[0] = buf_size;
    outBufDesc.numBufs = 1;
    outBufDesc.bufs = (XDAS_Int8 **) &buf;
    outBufDesc.bufSizes = outBufSizeArray;

    inArgs.size = sizeof(IH264VENC_InArgs);
    inArgs.inputID = 1;
    inArgs.topFieldFirstFlag = 1;

    inH264Args.videncInArgs = inArgs;
    inH264Args.insertUserData = 0;
    inH264Args.lengthUserData = 0;
    inH264Args.numOutputDataUnits = 0;

    outArgs.size = sizeof(VIDENC1_OutArgs);

    status = VIDENC1_process(hEncode, &inBufDesc, &outBufDesc,
            (VIDENC1_InArgs *) &inH264Args, &outArgs);
    if (status != VIDENC1_EOK)
        return -1;

    av_log(avctx, AV_LOG_DEBUG, "bytes generated: %d\n", (int) outArgs.bytesGenerated);

    switch (outArgs.encodedFrameType) {
    case IVIDEO_IDR_FRAME:
    case IVIDEO_I_FRAME:
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

static int dm365_encode_frame(AVCodecContext *avctx, uint8_t *buf,
        int buf_size, void *data)
{
    DM365Context *ctx = avctx->priv_data;

    return encoder_process(ctx->hEncode, avctx, buf, buf_size, data);
}

AVCodec ff_libdm365_h264_encoder =
{
    .name           = "libdm365_h264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .priv_data_size = sizeof(DM365Context),
    .init           = dm365_encode_init,
    .close          = dm365_encode_close,
    .encode         = dm365_encode_frame,
    .capabilities   = CODEC_CAP_EXPERIMENTAL | CODEC_CAP_DR1,
    .pix_fmts       = (const enum PixelFormat[]) {PIX_FMT_NV12, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("h.264 hardware encoder on dm365 SoC"),
};
