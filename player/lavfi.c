/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <assert.h>

#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "common/common.h"
#include "common/av_common.h"
#include "common/msg.h"

#include "audio/format.h"
#include "audio/aframe.h"
#include "video/mp_image.h"
#include "audio/fmt-conversion.h"
#include "video/fmt-conversion.h"
#include "video/hwdec.h"

#include "lavfi.h"

#if LIBAVFILTER_VERSION_MICRO < 100
#define av_buffersink_get_frame_flags(a, b, c) av_buffersink_get_frame(a, b)
#define AV_BUFFERSINK_FLAG_NO_REQUEST 0
#endif

struct lavfi {
    struct mp_log *log;
    char *graph_string;

    struct mp_hwdec_devices *hwdec_devs;

    AVFilterGraph *graph;
    // Set to true once all inputs have been initialized, and the graph is
    // linked.
    bool initialized;

    // Set if all inputs have been marked as LAVFI_WAIT (except LAVFI_EOF pads).
    bool all_waiting;

    // Graph is draining to undo previously sent EOF. (If a stream leaves EOF
    // state, the graph needs to be recreated to "unstuck" it.)
    bool draining_recover_eof;
    // Graph is draining for format changes.
    bool draining_new_format;

    // Filter can't be put into a working state.
    bool failed;

    struct lavfi_pad **pads;
    int num_pads;

    AVFrame *tmp_frame;
};

struct lavfi_pad {
    struct lavfi *main;
    enum stream_type type;
    enum lavfi_direction dir;
    char *name; // user-given pad name

    bool connected;     // if false, inputs/otuputs are considered always EOF

    AVFilterContext *filter;
    int filter_pad;
    // buffersrc or buffersink connected to filter/filter_pad
    AVFilterContext *buffer;
    AVRational timebase;
    bool buffer_is_eof; // received/sent EOF to the buffer

    // 1-frame queue (used for both input and output)
    struct mp_image *pending_v;
    struct mp_aframe *pending_a;

    // -- dir==LAVFI_IN

    bool input_needed;  // filter has signaled it needs new input
    bool input_waiting; // caller notified us that it will feed after a wakeup
    bool input_again;   // caller wants us to feed data in the next iteration
    bool input_eof;     // caller notified us that no input will come anymore

    // used to check for format changes manually
    struct mp_image *in_fmt_v;
    struct mp_aframe *in_fmt_a;

    // -- dir==LAVFI_OUT

    bool output_needed; // caller has signaled it needs new output
    bool output_eof;    // last filter output was EOF
};

static void add_pad(struct lavfi *c, enum lavfi_direction dir, AVFilterInOut *item)
{
    int type = -1;
    enum AVMediaType avmt;
    if (dir == LAVFI_IN) {
        avmt = avfilter_pad_get_type(item->filter_ctx->input_pads, item->pad_idx);
    } else {
        avmt = avfilter_pad_get_type(item->filter_ctx->output_pads, item->pad_idx);
    }
    switch (avmt) {
    case AVMEDIA_TYPE_VIDEO: type = STREAM_VIDEO; break;
    case AVMEDIA_TYPE_AUDIO: type = STREAM_AUDIO; break;
    default: abort();
    }

    if (!item->name) {
        MP_FATAL(c, "filter pad without name label\n");
        c->failed = true;
        return;
    }

    struct lavfi_pad *p = lavfi_find_pad(c, item->name);
    if (p) {
        // Graph recreation case: reassociate an existing pad.
        if (p->dir != dir || p->type != type) {
            MP_FATAL(c, "pad '%s' changed type or direction\n", item->name);
            c->failed = true;
            return;
        }
    } else {
        p = talloc_zero(c, struct lavfi_pad);
        p->main = c;
        p->dir = dir;
        p->name = talloc_strdup(p, item->name);
        p->type = type;
        MP_TARRAY_APPEND(c, c->pads, c->num_pads, p);
    }
    p->filter = item->filter_ctx;
    p->filter_pad = item->pad_idx;
}

static void add_pads(struct lavfi *c, enum lavfi_direction dir, AVFilterInOut *list)
{
    for (; list; list = list->next)
        add_pad(c, dir, list);
}

// Parse the user-provided filter graph, and populate the unlinked filter pads.
static void precreate_graph(struct lavfi *c)
{
    assert(!c->graph);
    c->graph = avfilter_graph_alloc();
    if (!c->graph)
        abort();
    AVFilterInOut *in = NULL, *out = NULL;
    if (avfilter_graph_parse2(c->graph, c->graph_string, &in, &out) < 0) {
        c->graph = NULL;
        MP_FATAL(c, "parsing the filter graph failed\n");
        c->failed = true;
        return;
    }
    add_pads(c, LAVFI_IN, in);
    add_pads(c, LAVFI_OUT, out);
    avfilter_inout_free(&in);
    avfilter_inout_free(&out);

    // Now check for pads which could not be reassociated.
    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        // ok, not much we can do
        if (!pad->filter)
            MP_FATAL(c, "filter pad '%s' can not be reconnected\n", pad->name);
    }
}

static void free_graph(struct lavfi *c)
{
    avfilter_graph_free(&c->graph);
    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        pad->filter = NULL;
        pad->filter_pad = -1;
        pad->buffer = NULL;
        TA_FREEP(&pad->in_fmt_v);
        TA_FREEP(&pad->in_fmt_a);
        pad->buffer_is_eof = false;
        pad->input_needed = false;
        pad->input_waiting = false;
        pad->input_again = false;
        pad->input_eof = false;
        pad->output_needed = false;
        pad->output_eof = false;
    }
    c->initialized = false;
    c->all_waiting = false;
    c->draining_recover_eof = false;
    c->draining_new_format = false;
}

static void drop_pad_data(struct lavfi_pad *pad)
{
    talloc_free(pad->pending_a);
    pad->pending_a = NULL;
    talloc_free(pad->pending_v);
    pad->pending_v = NULL;
}

static void clear_data(struct lavfi *c)
{
    for (int n = 0; n < c->num_pads; n++)
        drop_pad_data(c->pads[n]);
}

void lavfi_seek_reset(struct lavfi *c)
{
    free_graph(c);
    clear_data(c);
    precreate_graph(c);
}

struct lavfi *lavfi_create(struct mp_log *log, char *graph_string)
{
    struct lavfi *c = talloc_zero(NULL, struct lavfi);
    c->log = log;
    c->graph_string = graph_string;
    c->tmp_frame = av_frame_alloc();
    if (!c->tmp_frame)
        abort();
    precreate_graph(c);
    return c;
}

void lavfi_destroy(struct lavfi *c)
{
    if (!c)
        return;
    free_graph(c);
    clear_data(c);
    av_frame_free(&c->tmp_frame);
    talloc_free(c);
}

const char *lavfi_get_graph(struct lavfi *c)
{
    return c->graph_string;
}

struct lavfi_pad *lavfi_find_pad(struct lavfi *c, char *name)
{
    for (int n = 0; n < c->num_pads; n++) {
        if (strcmp(c->pads[n]->name, name) == 0)
            return c->pads[n];
    }
    return NULL;
}

enum lavfi_direction lavfi_pad_direction(struct lavfi_pad *pad)
{
    return pad->dir;
}

enum stream_type lavfi_pad_type(struct lavfi_pad *pad)
{
    return pad->type;
}

void lavfi_set_connected(struct lavfi_pad *pad, bool connected)
{
    pad->connected = connected;
    if (!pad->connected) {
        pad->output_needed = false;
        drop_pad_data(pad);
    }
}

bool lavfi_get_connected(struct lavfi_pad *pad)
{
    return pad->connected;
}

// Ensure to send EOF to each input pad, so the graph can be drained properly.
static void send_global_eof(struct lavfi *c)
{
    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        if (!pad->buffer || pad->dir != LAVFI_IN || pad->buffer_is_eof)
            continue;

        if (av_buffersrc_add_frame(pad->buffer, NULL) < 0)
            MP_FATAL(c, "could not send EOF to filter\n");

        pad->buffer_is_eof = true;
    }
}

// libavfilter allows changing some parameters on the fly, but not
// others.
static bool is_aformat_ok(struct mp_aframe *a, struct mp_aframe *b)
{
    struct mp_chmap ca = {0}, cb = {0};
    mp_aframe_get_chmap(a, &ca);
    mp_aframe_get_chmap(b, &cb);
    return mp_chmap_equals(&ca, &cb) &&
           mp_aframe_get_rate(a) == mp_aframe_get_rate(b) &&
           mp_aframe_get_format(a) == mp_aframe_get_format(b);
}
static bool is_vformat_ok(struct mp_image *a, struct mp_image *b)
{
    return a->imgfmt == b->imgfmt &&
           a->w == b->w && a->h && b->h &&
           a->params.p_w == b->params.p_w && a->params.p_h == b->params.p_h;
}

static void check_format_changes(struct lavfi *c)
{
    // check each pad for new input format
    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        if (!pad->buffer || pad->dir != LAVFI_IN)
            continue;

        if (pad->type == STREAM_AUDIO && pad->pending_a && pad->in_fmt_a) {
            c->draining_new_format |= !is_aformat_ok(pad->pending_a,
                                                     pad->in_fmt_a);
        }
        if (pad->type == STREAM_VIDEO && pad->pending_v && pad->in_fmt_v) {
            c->draining_new_format |= !is_vformat_ok(pad->pending_v,
                                                     pad->in_fmt_v);
        }
    }

    if (c->initialized && c->draining_new_format)
        send_global_eof(c);
}

// Attempt to initialize all pads. Return true if all are initialized, or
// false if more data is needed (or on error).
static bool init_pads(struct lavfi *c)
{
    if (!c->graph)
        goto error;

    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        if (pad->buffer)
            continue;

        if (!pad->filter)
            goto error; // can happen if pad reassociation fails

        if (pad->dir == LAVFI_OUT) {
            AVFilter *dst_filter = NULL;
            if (pad->type == STREAM_AUDIO) {
                dst_filter = avfilter_get_by_name("abuffersink");
            } else if (pad->type == STREAM_VIDEO) {
                dst_filter = avfilter_get_by_name("buffersink");
            } else {
                assert(0);
            }

            if (!dst_filter)
                goto error;

            char name[256];
            snprintf(name, sizeof(name), "mpv_sink_%s", pad->name);

            if (avfilter_graph_create_filter(&pad->buffer, dst_filter,
                                             name, NULL, NULL, c->graph) < 0)
                goto error;

            if (avfilter_link(pad->filter, pad->filter_pad, pad->buffer, 0) < 0)
                goto error;
        } else {
            TA_FREEP(&pad->in_fmt_v); // potentially cleanup previous error state

            pad->input_eof |= !pad->connected;

            if (pad->pending_a) {
                assert(pad->type == STREAM_AUDIO);
                pad->in_fmt_a = mp_aframe_new_ref(pad->pending_a);
                if (!pad->in_fmt_a)
                    goto error;
                mp_aframe_unref_data(pad->in_fmt_a);
            } else if (pad->pending_v) {
                assert(pad->type == STREAM_VIDEO);
                pad->in_fmt_v = mp_image_new_ref(pad->pending_v);
                if (!pad->in_fmt_v)
                    goto error;
                mp_image_unref_data(pad->in_fmt_v);
            } else if (pad->input_eof) {
                // libavfilter makes this painful. Init it with a dummy config,
                // just so we can tell it the stream is EOF.
                if (pad->type == STREAM_AUDIO) {
                    pad->in_fmt_a = mp_aframe_create();
                    mp_aframe_set_format(pad->in_fmt_a, AF_FORMAT_FLOAT);
                    mp_aframe_set_chmap(pad->in_fmt_a,
                                        &(struct mp_chmap)MP_CHMAP_INIT_STEREO);
                    mp_aframe_set_rate(pad->in_fmt_a, 48000);
                } else if (pad->type == STREAM_VIDEO) {
                    pad->in_fmt_v = talloc_zero(NULL, struct mp_image);
                    mp_image_setfmt(pad->in_fmt_v, IMGFMT_420P);
                    mp_image_set_size(pad->in_fmt_v, 64, 64);
                }
            } else {
                // no input data, format unknown, can't init, wait longer.
                pad->input_needed = true;
                return false;
            }

            AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
            if (!params)
                goto error;

            char *filter_name = NULL;
            if (pad->type == STREAM_AUDIO) {
                params->time_base = pad->timebase =
                    (AVRational){1, mp_aframe_get_rate(pad->in_fmt_a)};
                params->format =
                    af_to_avformat(mp_aframe_get_format(pad->in_fmt_a));
                params->sample_rate = mp_aframe_get_rate(pad->in_fmt_a);
                struct mp_chmap chmap = {0};
                mp_aframe_get_chmap(pad->in_fmt_a, &chmap);
                params->channel_layout = mp_chmap_to_lavc(&chmap);
                filter_name = "abuffer";
            } else if (pad->type == STREAM_VIDEO) {
                params->time_base = pad->timebase = AV_TIME_BASE_Q;
                params->format = imgfmt2pixfmt(pad->in_fmt_v->imgfmt);
                params->width = pad->in_fmt_v->w;
                params->height = pad->in_fmt_v->h;
                params->sample_aspect_ratio.num = pad->in_fmt_v->params.p_w;
                params->sample_aspect_ratio.den = pad->in_fmt_v->params.p_h;
                params->hw_frames_ctx = pad->in_fmt_v->hwctx;
                filter_name = "buffer";
            } else {
                assert(0);
            }

            AVFilter *filter = avfilter_get_by_name(filter_name);
            if (filter) {
                char name[256];
                snprintf(name, sizeof(name), "mpv_src_%s", pad->name);

                pad->buffer = avfilter_graph_alloc_filter(c->graph, filter, name);
            }
            if (!pad->buffer) {
                av_free(params);
                goto error;
            }

            if (pad->type == STREAM_AUDIO) {
                char layout[80];
                snprintf(layout, sizeof(layout), "%lld",
                         (long long)params->channel_layout);
                av_opt_set(pad->buffer, "channel_layout", layout,
                           AV_OPT_SEARCH_CHILDREN);
            }

            int ret = av_buffersrc_parameters_set(pad->buffer, params);
            av_free(params);
            if (ret < 0)
                goto error;

            if (avfilter_init_str(pad->buffer, NULL) < 0)
                goto error;

            if (avfilter_link(pad->buffer, 0, pad->filter, pad->filter_pad) < 0)
                goto error;
        }
    }

    return true;
error:
    MP_FATAL(c, "could not initialize filter pads\n");
    c->failed = true;
    return false;
}

static void dump_graph(struct lavfi *c)
{
#if LIBAVFILTER_VERSION_MICRO >= 100
    MP_VERBOSE(c, "Filter graph:\n");
    char *s = avfilter_graph_dump(c->graph, NULL);
    if (s)
        MP_VERBOSE(c, "%s\n", s);
    av_free(s);
#endif
}

void lavfi_pad_set_hwdec_devs(struct lavfi_pad *pad,
                              struct mp_hwdec_devices *hwdevs)
{
    // We don't actually treat this per-pad.
    pad->main->hwdec_devs = hwdevs;
}

// Initialize the graph if all inputs have formats set. If it's already
// initialized, or can't be initialized yet, do nothing.
static void init_graph(struct lavfi *c)
{
    assert(!c->initialized);

    if (init_pads(c)) {
        if (c->hwdec_devs) {
            struct mp_hwdec_ctx *hwdec = hwdec_devices_get_first(c->hwdec_devs);
            for (int n = 0; n < c->graph->nb_filters; n++) {
                AVFilterContext *filter = c->graph->filters[n];
                if (hwdec && hwdec->av_device_ref)
                    filter->hw_device_ctx = av_buffer_ref(hwdec->av_device_ref);
            }
        }

        // And here the actual libavfilter initialization happens.
        if (avfilter_graph_config(c->graph, NULL) < 0) {
            MP_FATAL(c, "failed to configure the filter graph\n");
            free_graph(c);
            c->failed = true;
            return;
        }

        for (int n = 0; n < c->num_pads; n++) {
            struct lavfi_pad *pad = c->pads[n];
            if (pad->dir == LAVFI_OUT)
                pad->timebase = pad->buffer->inputs[0]->time_base;
        }

        c->initialized = true;

        dump_graph(c);
    }
}

static void feed_input_pads(struct lavfi *c)
{
    assert(c->initialized);

    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];
        if (pad->dir != LAVFI_IN)
            continue;

        pad->input_needed = false;
        pad->input_eof |= !pad->connected;

#if LIBAVFILTER_VERSION_MICRO >= 100
        if (!av_buffersrc_get_nb_failed_requests(pad->buffer))
            continue;
#endif

        if (c->draining_recover_eof || c->draining_new_format)
            continue;

        if (pad->buffer_is_eof)
            continue;

        AVFrame *frame = NULL;
        double pts = 0;
        bool eof = false;
        if (pad->pending_v) {
            pts = pad->pending_v->pts;
            frame = mp_image_to_av_frame_and_unref(pad->pending_v);
            pad->pending_v = NULL;
        } else if (pad->pending_a) {
            pts = mp_aframe_get_pts(pad->pending_a);
            frame = mp_aframe_to_avframe_and_unref(pad->pending_a);
            pad->pending_a = NULL;
        } else {
            if (!pad->input_eof) {
                pad->input_needed = true;
                continue;
            }
            eof = true;
        }

        if (!frame && !eof) {
            MP_FATAL(c, "out of memory or unsupported format\n");
            continue;
        }

        if (frame)
            frame->pts = mp_pts_to_av(pts, &pad->timebase);

        pad->buffer_is_eof = !frame;

        if (av_buffersrc_add_frame(pad->buffer, frame) < 0)
            MP_FATAL(c, "could not pass frame to filter\n");
        av_frame_free(&frame);

        pad->input_again = false;
        pad->input_eof = eof;
        pad->input_waiting = eof; // input _might_ come again in the future
    }
}

static void read_output_pads(struct lavfi *c)
{
    assert(c->initialized);

    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];

        if (pad->dir != LAVFI_OUT)
            continue;

        // If disconnected, read and discard everything (passively).
        if (pad->connected && !pad->output_needed)
            continue;

        assert(pad->buffer);
        assert(!pad->pending_v && !pad->pending_a);

        int flags = pad->output_needed ? 0 : AV_BUFFERSINK_FLAG_NO_REQUEST;
        int r = AVERROR_EOF;
        if (!pad->buffer_is_eof)
            r = av_buffersink_get_frame_flags(pad->buffer, c->tmp_frame, flags);
        if (r >= 0) {
            pad->output_needed = false;
            double pts = mp_pts_from_av(c->tmp_frame->pts, &pad->timebase);
            if (pad->type == STREAM_AUDIO) {
                pad->pending_a = mp_aframe_from_avframe(c->tmp_frame);
                if (pad->pending_a)
                    mp_aframe_set_pts(pad->pending_a, pts);
            } else if (pad->type == STREAM_VIDEO) {
                pad->pending_v = mp_image_from_av_frame(c->tmp_frame);
                if (pad->pending_v)
                    pad->pending_v->pts = pts;
            } else {
                assert(0);
            }
            av_frame_unref(c->tmp_frame);
            if (!pad->pending_v && !pad->pending_a)
                MP_ERR(c, "could not use filter output\n");
            pad->output_eof = false;
            if (!pad->connected)
                drop_pad_data(pad);
        } else if (r == AVERROR(EAGAIN)) {
            // We expect that libavfilter will request input on one of the
            // input pads (via av_buffersrc_get_nb_failed_requests()).
            pad->output_eof = false;
        } else if (r == AVERROR_EOF) {
            pad->output_needed = false;
            pad->buffer_is_eof = true;
            if (!c->draining_recover_eof && !c->draining_new_format)
                pad->output_eof = true;
        } else {
            // Real error - ignore it.
            MP_ERR(c, "error on filtering (%d)\n", r);
        }
    }
}

// Process filter input and outputs. Return if progress was made (then the
// caller should repeat). If it returns false, the caller should go to sleep
// (as all inputs are asleep as well and no further output can be produced).
bool lavfi_process(struct lavfi *c)
{
    check_format_changes(c);

    if (!c->initialized)
        init_graph(c);

    if (c->initialized) {
        read_output_pads(c);
        feed_input_pads(c);
    }

    bool all_waiting = true;
    bool any_needs_input = false;
    bool any_needs_output = false;
    bool all_output_eof = true;
    bool all_input_eof = true;

    // Determine the graph state
    for (int n = 0; n < c->num_pads; n++) {
        struct lavfi_pad *pad = c->pads[n];

        if (pad->dir == LAVFI_IN) {
            all_waiting &= pad->input_waiting;
            any_needs_input |= pad->input_needed;
            all_input_eof &= pad->input_eof;
        } else if (pad->dir == LAVFI_OUT) {
            all_output_eof &= pad->buffer_is_eof;
            any_needs_output |= pad->output_needed;
        }
    }

    if (all_output_eof && !all_input_eof) {
        free_graph(c);
        precreate_graph(c);
        all_waiting = false;
        any_needs_input = true;
    }

    c->all_waiting = all_waiting;
    return (any_needs_input || any_needs_output) && !all_waiting;
}

bool lavfi_has_failed(struct lavfi *c)
{
    return c->failed;
}

// Request an output frame on this output pad.
// Returns req_status
static int lavfi_request_frame(struct lavfi_pad *pad)
{
    assert(pad->dir == LAVFI_OUT);

    if (pad->main->failed)
        return DATA_EOF;

    if (!(pad->pending_a || pad->pending_v)) {
        pad->output_needed = true;
        lavfi_process(pad->main);
    }

    if (pad->pending_a || pad->pending_v) {
        return DATA_OK;
    } else if (pad->output_eof) {
        return DATA_EOF;
    } else if (pad->main->all_waiting) {
        return DATA_WAIT;
    }
    return DATA_STARVE;
}

// Try to read a new frame from an output pad. Returns one of the following:
//      DATA_OK: a frame is returned
//      DATA_STARVE: needs more input data
//      DATA_WAIT: needs more input data, and all inputs in LAVFI_WAIT state
//      DATA_EOF: no more data
int lavfi_request_frame_a(struct lavfi_pad *pad, struct mp_aframe **out_aframe)
{
    int r = lavfi_request_frame(pad);
    *out_aframe = pad->pending_a;
    pad->pending_a = NULL;
    return r;
}

// See lavfi_request_frame_a() for remarks.
int lavfi_request_frame_v(struct lavfi_pad *pad, struct mp_image **out_vframe)
{
    int r = lavfi_request_frame(pad);
    *out_vframe = pad->pending_v;
    pad->pending_v = NULL;
    return r;
}

bool lavfi_needs_input(struct lavfi_pad *pad)
{
    assert(pad->dir == LAVFI_IN);
    lavfi_process(pad->main);
    return pad->input_needed;
}

// A filter user is supposed to call lavfi_needs_input(), and if that returns
// true, send either a new status or a frame. A status can be one of:
//      DATA_STARVE: a new frame/status will come, caller will retry
//      DATA_WAIT: a new frame/status will come, but caller goes to sleep
//      DATA_EOF: no more input possible (in near time)
// If you have a new frame, use lavfi_send_frame_ instead.
// Calling this without lavfi_needs_input() returning true before is not
// allowed.
void lavfi_send_status(struct lavfi_pad *pad, int status)
{
    assert(pad->dir == LAVFI_IN);
    assert(pad->input_needed);
    assert(status != DATA_OK);
    assert(!pad->pending_v && !pad->pending_a);

    pad->input_waiting = status == DATA_WAIT || status == DATA_EOF;
    pad->input_again = status == DATA_STARVE;
    pad->input_eof = status == DATA_EOF;
}

static void lavfi_sent_frame(struct lavfi_pad *pad)
{
    assert(pad->dir == LAVFI_IN);
    assert(pad->input_needed);
    assert(pad->pending_a || pad->pending_v);
    pad->input_waiting = pad->input_again = pad->input_eof = false;
    pad->input_needed = false;
}

// See lavfi_send_status() for remarks.
void lavfi_send_frame_a(struct lavfi_pad *pad, struct mp_aframe *aframe)
{
    assert(pad->type == STREAM_AUDIO);
    assert(!pad->pending_a);
    pad->pending_a = aframe;
    lavfi_sent_frame(pad);
}

// See lavfi_send_status() for remarks.
void lavfi_send_frame_v(struct lavfi_pad *pad, struct mp_image *vframe)
{
    assert(pad->type == STREAM_VIDEO);
    assert(!pad->pending_v);
    pad->pending_v = vframe;
    lavfi_sent_frame(pad);
}

