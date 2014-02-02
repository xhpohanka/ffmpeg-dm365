/*
 * Copyright (c) 2014 Jan Pohanka <xhpohanka gmail com>
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
 * Draws a box around areas marked in comment metadata
 * Based on drawbox filter
 */

#include "libavutil/colorspace.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/eval.h"
#include "libavutil/pixdesc.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#include <string.h>

static const char *const var_names[] = {
    "dar",
    "hsub", "vsub",
    "sar",
    "t",
    NULL
};

enum { Y, U, V, A };

enum var_name {
    VAR_DAR,
    VAR_HSUB, VAR_VSUB,
    VAR_SAR,
    VAR_T,
    VARS_NB
};

typedef struct {
    const AVClass *class;
    int thickness;
    char *color_str;
    unsigned char yuv_color[4];
    int invert_color; ///< invert luma color
    int vsub, hsub;   ///< chroma subsampling
    char *t_expr;          ///< expression for thickness
    int *mip_info;
} DrawBoxContext;

static const int NUM_EXPR_EVALS = 2;

static av_cold int init(AVFilterContext *ctx)
{
    DrawBoxContext *s = ctx->priv;
    uint8_t rgba_color[4];

    if (!strcmp(s->color_str, "invert"))
        s->invert_color = 1;
    else if (av_parse_color(rgba_color, s->color_str, -1, ctx) < 0)
        return AVERROR(EINVAL);

    if (!s->invert_color) {
        s->yuv_color[Y] = RGB_TO_Y_CCIR(rgba_color[0], rgba_color[1], rgba_color[2]);
        s->yuv_color[U] = RGB_TO_U_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
        s->yuv_color[V] = RGB_TO_V_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
        s->yuv_color[A] = rgba_color[3];
    }

    s->mip_info = av_malloc(38 * 8 * sizeof(int));
    if (!s->mip_info)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
	DrawBoxContext *s = ctx->priv;

	av_free(s->mip_info);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV411P,  AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV440P,  AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DrawBoxContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    double var_values[VARS_NB], res;
    char *expr;
    int ret;
    int i;

    s->hsub = desc->log2_chroma_w;
    s->vsub = desc->log2_chroma_h;

    var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ? av_q2d(inlink->sample_aspect_ratio) : 1;
    var_values[VAR_DAR]  = (double)inlink->w / inlink->h * var_values[VAR_SAR];
    var_values[VAR_HSUB] = s->hsub;
    var_values[VAR_VSUB] = s->vsub;
    var_values[VAR_T] = NAN;

    for (i = 0; i <= NUM_EXPR_EVALS; i++) {
        /* evaluate expressions, fail on last iteration */
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->t_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->thickness = var_values[VAR_T] = res;
    }

    av_log(ctx, AV_LOG_VERBOSE, "color:0x%02X%02X%02X%02X\n",
           s->yuv_color[Y], s->yuv_color[U], s->yuv_color[V], s->yuv_color[A]);

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n",
           expr);
    return ret;
}

static int read_mip_info(char *str, int *mip_info)
{
    char *tmp;
    char *endp;
    unsigned char nums[44];
    int i, len;

    len = strlen(str);
    if (len != 227)
        return -1;

    endp = str + len ;
    tmp = str + 8*5; //first 8 bits are not interesting for us
    for (i = 0; tmp < endp; i++) {
        nums[i] = (unsigned char) atoi(tmp);
        tmp += 5;
    }

    for (i = 0; i < 38; i++) {
        int j;
        for (j = 0; j < 8; j++)
            *mip_info++ = !!(nums[i] & (1 << j));
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    DrawBoxContext *s = inlink->dst->priv;
    AVDictionaryEntry *e;
    int ret;
    int i;

    e = av_dict_get(*avpriv_frame_get_metadatap(frame), "comment", NULL, AV_DICT_IGNORE_SUFFIX);
    if (!e) {
        av_log(inlink->dst, AV_LOG_WARNING, "frame does not have MIP info");
        goto end_frame;
    }

    ret = read_mip_info(e->value, s->mip_info);
    if (ret < 0) {
        av_log(inlink->dst, AV_LOG_WARNING, "corrupted MIP info");
        goto end_frame;
    }


    for (i = 0; i < 15*20; i++) {
        int plane, x, y, xb, yb, w, h;
        unsigned char *row[4];

        if (s->mip_info[i] == 0)
            continue;

        xb = frame->width / 20 * (i % 20);
        yb = frame->height / 15 * (i / 20);
        w = frame->width/20;
        h = frame->height/15;
        for (y = FFMAX(yb, 0); y < frame->height && y < (yb + h); y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                frame->linesize[plane] * (y >> s->vsub);

            if (s->invert_color) {
                for (x = FFMAX(xb, 0); x < xb + w && x < frame->width; x++)
                    if ((y - yb < s->thickness) || (yb + h - 1 - y < s->thickness) ||
                            (x - xb < s->thickness) || (xb + w - 1 - x < s->thickness))
                        row[0][x] = 0xff - row[0][x];
            } else {
                for (x = FFMAX(xb, 0); x < xb + w && x < frame->width; x++) {
                    double alpha = (double)s->yuv_color[A] / 255;

                    if ((y - yb < s->thickness) || (yb + h - 1 - y < s->thickness) ||
                            (x - xb < s->thickness) || (xb + w - 1 - x < s->thickness)) {
                        row[0][x                 ] = (1 - alpha) * row[0][x                 ] + alpha * s->yuv_color[Y];
                        row[1][x >> s->hsub] = (1 - alpha) * row[1][x >> s->hsub] + alpha * s->yuv_color[U];
                        row[2][x >> s->hsub] = (1 - alpha) * row[2][x >> s->hsub] + alpha * s->yuv_color[V];
                    }
                }
            }
        }
    }

    end_frame:
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(DrawBoxContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#if CONFIG_JCSHOWMOTION_FILTER

static const AVOption jcshowmotion_options[] = {
    { "color",     "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, CHAR_MIN, CHAR_MAX, FLAGS },
    { "c",         "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, CHAR_MIN, CHAR_MAX, FLAGS },
    { "thickness", "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "t",         "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(jcshowmotion);

static const AVFilterPad jcshowmotion_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_input,
        .filter_frame   = filter_frame,
        .needs_writable = 1,
    },
    { NULL }
};

static const AVFilterPad jcshowmotion_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_jcshowmotion = {
    .name          = "jcshowmotion",
    .description   = NULL_IF_CONFIG_SMALL("Draw a colored box on the input video."),
    .priv_size     = sizeof(DrawBoxContext),
    .priv_class    = &jcshowmotion_class,
    .init          = init,
    .uninit		   = uninit,
    .query_formats = query_formats,
    .inputs        = jcshowmotion_inputs,
    .outputs       = jcshowmotion_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
#endif /* CONFIG_JCSHOWMOTION_FILTER */
