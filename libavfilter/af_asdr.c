/*
 * Copyright (c) 2021 Paul B Mahol
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

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"

#include "avfilter.h"
#include "filters.h"
#include "internal.h"

typedef struct AudioSDRContext {
    int channels;
    double *sum_u;
    double *sum_uv;

    AVFrame *cache[2];

    int (*filter)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
} AudioSDRContext;

#define SDR_FILTER(name, type)                                                \
static int sdr_##name(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)\
{                                                                             \
    AudioSDRContext *s = ctx->priv;                                           \
    AVFrame *u = s->cache[0];                                                 \
    AVFrame *v = s->cache[1];                                                 \
    const int channels = u->ch_layout.nb_channels;                            \
    const int start = (channels * jobnr) / nb_jobs;                           \
    const int end = (channels * (jobnr+1)) / nb_jobs;                         \
    const int nb_samples = u->nb_samples;                                     \
                                                                              \
    for (int ch = start; ch < end; ch++) {                                    \
        const type *const us = (type *)u->extended_data[ch];                  \
        const type *const vs = (type *)v->extended_data[ch];                  \
        double sum_uv = 0.;                                                   \
        double sum_u = 0.;                                                    \
                                                                              \
        for (int n = 0; n < nb_samples; n++) {                                \
            sum_u  += us[n] * us[n];                                          \
            sum_uv += (us[n] - vs[n]) * (us[n] - vs[n]);                      \
        }                                                                     \
                                                                              \
        s->sum_uv[ch] += sum_uv;                                              \
        s->sum_u[ch]  += sum_u;                                               \
    }                                                                         \
                                                                              \
    return 0;                                                                 \
}

SDR_FILTER(fltp, float)
SDR_FILTER(dblp, double)

static int activate(AVFilterContext *ctx)
{
    AudioSDRContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret, status, available;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, ctx);

    available = FFMIN(ff_inlink_queued_samples(ctx->inputs[0]), ff_inlink_queued_samples(ctx->inputs[1]));
    if (available > 0) {
        AVFrame *out;

        for (int i = 0; i < 2; i++) {
            ret = ff_inlink_consume_samples(ctx->inputs[i], available, available, &s->cache[i]);
            if (ret < 0) {
                av_frame_free(&s->cache[0]);
                av_frame_free(&s->cache[1]);
                return ret;
            }
        }

        if (!ctx->is_disabled)
            ff_filter_execute(ctx, s->filter, NULL, NULL,
                              FFMIN(outlink->ch_layout.nb_channels, ff_filter_get_nb_threads(ctx)));

        av_frame_free(&s->cache[1]);
        out = s->cache[0];
        s->cache[0] = NULL;

        return ff_filter_frame(outlink, out);
    }

    for (int i = 0; i < 2; i++) {
        if (ff_inlink_acknowledge_status(ctx->inputs[i], &status, &pts)) {
            ff_outlink_set_status(outlink, status, pts);
            return 0;
        }
    }

    if (ff_outlink_frame_wanted(outlink)) {
        for (int i = 0; i < 2; i++) {
            if (ff_inlink_queued_samples(ctx->inputs[i]) > 0)
                continue;
            ff_inlink_request_frame(ctx->inputs[i]);
        }
        return 0;
    }

    return FFERROR_NOT_READY;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    AudioSDRContext *s = ctx->priv;

    s->channels = inlink->ch_layout.nb_channels;
    s->filter = inlink->format == AV_SAMPLE_FMT_FLTP ? sdr_fltp : sdr_dblp;

    s->sum_u  = av_calloc(outlink->ch_layout.nb_channels, sizeof(*s->sum_u));
    s->sum_uv = av_calloc(outlink->ch_layout.nb_channels, sizeof(*s->sum_uv));
    if (!s->sum_u || !s->sum_uv)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioSDRContext *s = ctx->priv;

    for (int ch = 0; ch < s->channels; ch++)
        av_log(ctx, AV_LOG_INFO, "SDR ch%d: %g dB\n", ch, 20. * log10(s->sum_u[ch] / s->sum_uv[ch]));

    av_frame_free(&s->cache[0]);
    av_frame_free(&s->cache[1]);

    av_freep(&s->sum_u);
    av_freep(&s->sum_uv);
}

static const AVFilterPad inputs[] = {
    {
        .name = "input0",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    {
        .name = "input1",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
};

const AVFilter ff_af_asdr = {
    .name           = "asdr",
    .description    = NULL_IF_CONFIG_SMALL("Measure Audio Signal-to-Distortion Ratio."),
    .priv_size      = sizeof(AudioSDRContext),
    .activate       = activate,
    .uninit         = uninit,
    .flags          = AVFILTER_FLAG_METADATA_ONLY |
                      AVFILTER_FLAG_SLICE_THREADS |
                      AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP,
                      AV_SAMPLE_FMT_DBLP),
};
