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
} DM365Context;

static av_cold int dm365_decode_init(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;

    return 0;
}

static av_cold int dm365_decode_close(AVCodecContext *avctx)
{
    DM365Context *ctx = avctx->priv_data;


    return 0;
}

static int dm365_decode_frame(AVCodecContext *avctx,
        void *data, int *data_size,
        AVPacket *avpkt)
{
    uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    DM365Context *ctx = avctx->priv_data;
    AVFrame *picture = &ctx->image, *output = data;


    return 0;
}

AVCodec ff_libdm365_h264_decoder =
{
    .name           = "libdm365_h264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .priv_data_size = sizeof(DM365Context),
    .init           = dm365_decode_init,
    .close          = dm365_decode_close,
    .decode         = dm365_decode_frame,
};
