/*
 * Hardware decoding support for DM365 SoC
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
#include <ti/sdo/ce/video2/viddec2.h>
#include <ti/sdo/codecs/h264dec/ih264vdec.h>
#include <ti/sdo/linuxutils/cmem/include/cmem.h>

static CMEM_AllocParams alloc_params = {
    .type = CMEM_HEAP,
    .flags = CMEM_NONCACHED,
    .alignment = 32,
};

typedef struct DM365Context {
    Engine_Handle hEngine;
    VIDDEC2_Handle hDecode;
    void *codecParams;
    void *codecDynParams;
    void *in_buf;
    void *out_buf;
    XDAS_Int32 minNumOutBufs;
    XDAS_Int32 minOutBufSize[4];
} DM365Context;

static const VIDDEC2_Params Vdec2_Params_DEFAULT = {
    sizeof(VIDDEC2_Params),             /* size */
    576,                                /* maxHeight */
    720,                                /* maxWidth */
    30000,                              /* maxFrameRate */
    6000000,                            /* maxBitRate */
    XDM_BYTE,                           /* dataEndianess */
    XDM_YUV_420SP,                      /* forceChromaFormat */
};

static const VIDDEC2_DynamicParams Vdec2_DynamicParams_DEFAULT = {
    sizeof(VIDDEC2_DynamicParams),      /* size */
    XDM_DECODE_AU,                      /* decodeHeader */
    0,                                  /* displayWidth */
    IVIDEO_NO_SKIP,                     /* frameSkipMode */
    IVIDDEC2_DISPLAY_ORDER,             /* frameOrder */
    0,                                  /* newFrameFlag */
    0,                                  /* mbDataFlag */
};

static VIDDEC2_Handle decoder_create(Engine_Handle hEngine, String codecName,
        VIDDEC2_Params *params,
        VIDDEC2_DynamicParams *dynParams)
{
    VIDDEC2_Handle         hDecode;
    VIDDEC2_Status         decStatus;
    XDAS_Int32             status;

    if (hEngine == NULL || codecName == NULL ||
        params == NULL || dynParams == NULL) {
        return NULL;
    }

    /* Create video decoder instance */
    hDecode = VIDDEC2_create(hEngine, (String) codecName, params);
    if (hDecode == NULL)
        return NULL;

    /* Set video decoder dynamic params */
    decStatus.data.buf = NULL;
    decStatus.size = sizeof(VIDDEC2_Status);
    status = VIDDEC2_control(hDecode, XDM_SETPARAMS, dynParams, &decStatus);

    if (status != VIDDEC2_EOK) {
        VIDDEC2_delete(hDecode);
        return NULL;
    }

    return hDecode;
}

/*
 * allocates codecParams and codecDynParams, free them later with av_free
 */
static int h264_dec_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    IH264VDEC_Params *h264Params;
    IH264VDEC_DynamicParams *h264DynParams;
    IVIDDEC2_Params *params;
    IVIDDEC2_DynamicParams *dynParams;

    h264Params = av_malloc(sizeof(IH264VDEC_Params));
    if (h264Params == NULL)
        return AVERROR(ENOMEM);

    h264DynParams = av_mallocz(sizeof(IH264VDEC_DynamicParams));
    if (h264DynParams == NULL) {
        av_free(h264Params);
        return AVERROR(ENOMEM);
    }

    /* set default params */
    *h264Params = IH264VDEC_PARAMS;
    h264Params->frame_closedloop_flag = 1;
    h264Params->levelLimit = LEVEL_4_2;
    h264Params->inputDataMode = IH264VDEC_TI_ENTIREFRAME;
    h264Params->sliceFormat = IH264VDEC_TI_BYTESTREAM;

    h264DynParams->resetHDVICPeveryFrame = 1;

    params = &h264Params->viddecParams;
    dynParams = &h264DynParams->viddecDynamicParams;

    *params = Vdec2_Params_DEFAULT;
    *dynParams = Vdec2_DynamicParams_DEFAULT;

    params->maxWidth = avctx->width;
    params->maxHeight = avctx->height;

    params->size = sizeof(IH264VDEC_Params);
    dynParams->size = sizeof(IH264VDEC_DynamicParams);

    ctx->codecParams = h264Params;
    ctx->codecDynParams  = h264DynParams;

    ctx->hDecode = decoder_create(ctx->hEngine, "h264dec",
            ctx->codecParams, ctx->codecDynParams);

    if (!ctx->hDecode) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create decoder\n");
        av_freep(&ctx->codecParams);
        av_freep(&ctx->codecDynParams);
        return -1;
    }

    return 0;
}

static av_cold int dm365_decode_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;
    XDAS_Int32 status;
    VIDDEC2_Status decStatus;
    int ret, i, buf_size;

    /*
     * CERuntime_init() has to be called from main application
     * as well as CERuntime_exit(). Otherwise other dm365 codec
     * initialization or deinitialization could break everything
     *
     * CMEM_init() and CMEM_exit() is implemented more reasonably as it counts
     * its users, so we just need to assure that calls to init and exit equals.
     */

    CMEM_init();

    ctx->hEngine = Engine_open("decode", NULL, NULL);
    if (ctx->hEngine == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open codec engine.\n");
        return AVERROR(1);
    }

    switch (avctx->codec_id) {
    case CODEC_ID_H264:
        ret = h264_dec_init(avctx);
        break;
    default:
        ret = -1;
        break;
    }

    if (ret < 0) {
        Engine_close(ctx->hEngine);
        return ret;
    }

    /* get output buffer requirements */
    decStatus.data.buf = NULL;
    decStatus.size = sizeof(VIDDEC2_Status);
    decStatus.maxNumDisplayBufs = 0;

    status = VIDDEC2_control(ctx->hDecode, XDM_GETBUFINFO,
            ctx->codecDynParams, &decStatus);
    if (status != VIDDEC2_EOK) {
        av_log(avctx, AV_LOG_ERROR, "XDM_GETBUFINFO control failed\n");
        ret = AVERROR(1);
        goto init_cleanup;
    }

    ctx->minNumOutBufs = decStatus.bufInfo.minNumOutBufs;
    memcpy(ctx->minOutBufSize, decStatus.bufInfo.minOutBufSize,
            4*decStatus.bufInfo.minNumOutBufs);

    buf_size = 0;
    for (i = 0; i < ctx->minNumOutBufs; i++) {
        buf_size += ctx->minOutBufSize[i];
    }

    /* allocate continous buffers */
    ctx->out_buf = CMEM_alloc(buf_size, &alloc_params);
    if (ctx->out_buf == NULL) {
        ret = AVERROR(ENOMEM);
        goto init_cleanup;
    }

    /* TODO: input buffer could be smaller */
    ctx->in_buf = CMEM_alloc(buf_size, &alloc_params);
    if (ctx->in_buf == NULL) {
        ret = AVERROR(ENOMEM);
        CMEM_free(ctx->out_buf, &alloc_params);
        goto init_cleanup;
    }

    avctx->pix_fmt = avctx->codec->pix_fmts[0];

    return 0;

init_cleanup:
    VIDDEC2_delete(ctx->hDecode);
    av_free(ctx->codecParams);
    av_free(ctx->codecDynParams);
    Engine_close(ctx->hEngine);
    return ret;
}

static av_cold int dm365_decode_close(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;

    VIDDEC2_delete(ctx->hDecode);
    av_free(ctx->codecParams);
    av_free(ctx->codecDynParams);
    Engine_close(ctx->hEngine);

    CMEM_free(ctx->out_buf, &alloc_params);
    CMEM_free(ctx->in_buf, &alloc_params);
    CMEM_exit();

    return 0;
}

static int dm365_decode_frame(AVCodecContext *avctx,
        void *outdata, int *outdata_size, AVPacket *avpkt)
{
    DM365Context *ctx = avctx->priv_data;
    AVFrame *picture = outdata;
    IVIDDEC2_InArgs inArgs;
    IVIDDEC2_OutArgs outArgs;
    XDAS_Int32 status;
    XDM1_BufDesc inBufDesc;
    XDM_BufDesc outBufDesc;
    XDAS_Int8 *outBufPtrArray[2];


    outBufPtrArray[0] = ctx->out_buf;
    outBufPtrArray[1] = outBufPtrArray[0] + ctx->minOutBufSize[0];

    outBufDesc.numBufs  = ctx->minNumOutBufs;
    outBufDesc.bufSizes = ctx->minOutBufSize;
    outBufDesc.bufs     = outBufPtrArray;

    /* copy input data to CMEM buffer */
    /* TODO: add size check */
    memcpy(ctx->in_buf, avpkt->data, avpkt->size);

    inBufDesc.numBufs           = 1;
    inBufDesc.descs[0].buf      = ctx->in_buf;
    inBufDesc.descs[0].bufSize  = avpkt->size;

    inArgs.size     = sizeof(VIDDEC2_InArgs);
    inArgs.numBytes = avpkt->size;
    inArgs.inputID  = 1;

    outArgs.size    = sizeof(VIDDEC2_OutArgs);

    status = VIDDEC2_process(ctx->hDecode, &inBufDesc, &outBufDesc,
            (VIDDEC2_InArgs *) &inArgs, (VIDDEC2_OutArgs *) &outArgs);
    if (status != VIDDEC2_EOK) {
        return AVERROR_INVALIDDATA;
    } else {
        IVIDEO1_BufDesc *bd = &outArgs.decodedBufs;

        picture->data[0] = bd->bufDesc[0].buf;
        picture->data[1] = bd->bufDesc[1].buf;
        picture->data[2] = NULL;
        picture->data[3] = NULL;
        picture->linesize[0] = bd->framePitch;
        picture->linesize[1] = bd->framePitch;
        picture->linesize[2] = 0;
        picture->linesize[3] = 0;
    }
    *outdata_size = sizeof(AVPicture);

    avpkt->size = outArgs.bytesConsumed;

    return avpkt->size;
}

#if CONFIG_LIBDM365_H264_DECODER
AVCodec ff_libdm365_h264_decoder =
{
    .name           = "libdm365_h264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .priv_data_size = sizeof(DM365Context),
    .init           = dm365_decode_init,
    .close          = dm365_decode_close,
    .decode         = dm365_decode_frame,
    .pix_fmts       = (const enum PixelFormat[]) {PIX_FMT_NV12, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("h.264 hardware decoder on dm365 SoC"),
};
#endif
