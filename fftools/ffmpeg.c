/* ffmpeg CLI
 *
 * Merged from:
 *   - fftools/ffmpeg_sched.c
 *   - fftools/ffmpeg_dec.c
 *   - fftools/ffmpeg_demux.c
 *   - fftools/ffmpeg_enc.c
 *   - fftools/ffmpeg_filter.c
 *   - fftools/ffmpeg_hw.c
 *   - fftools/ffmpeg_mux.c
 *   - fftools/ffmpeg_mux_init.c
 *   - fftools/ffmpeg_opt.c
 *   - fftools/ffmpeg.c
 */


/* ========== fftools/ffmpeg_sched.c ========== */

/*
 * Inter-thread scheduling/synchronization.
 * Copyright (c) 2023 Anton Khirnov
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

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cmdutils.h"
#include "ffmpeg_sched.h"
#include "ffmpeg_utils.h"
#include "sync_queue.h"
#include "thread_queue.h"

#include "libavcodec/packet.h"

#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/fifo.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"
#include "libavutil/time.h"

// 100 ms
// FIXME: some other value? make this dynamic?
#define SCHEDULE_TOLERANCE (100 * 1000)

enum QueueType {
    QUEUE_PACKETS,
    QUEUE_FRAMES,
};

typedef struct SchWaiter {
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
    atomic_int          choked;

    // the following are internal state of schedule_update_locked() and must not
    // be accessed outside of it
    int                 choked_prev;
    int                 choked_next;
} SchWaiter;

typedef struct SchTask {
    Scheduler          *parent;
    SchedulerNode       node;

    SchThreadFunc       func;
    void               *func_arg;

    pthread_t           thread;
    int                 thread_running;
} SchTask;

typedef struct SchDecOutput {
    SchedulerNode      *dst;
    uint8_t            *dst_finished;
    unsigned         nb_dst;
} SchDecOutput;

typedef struct SchDec {
    const AVClass      *class;

    SchedulerNode       src;

    SchDecOutput       *outputs;
    unsigned         nb_outputs;

    SchTask             task;
    // Queue for receiving input packets, one stream.
    ThreadQueue        *queue;

    // Queue for sending post-flush end timestamps back to the source
    AVThreadMessageQueue *queue_end_ts;
    int                 expect_end_ts;

    // temporary storage used by sch_dec_send()
    AVFrame            *send_frame;
} SchDec;

typedef struct SchSyncQueue {
    SyncQueue          *sq;
    AVFrame            *frame;
    pthread_mutex_t     lock;

    unsigned           *enc_idx;
    unsigned         nb_enc_idx;
} SchSyncQueue;

typedef struct SchEnc {
    const AVClass      *class;

    SchedulerNode       src;
    SchedulerNode      *dst;
    uint8_t            *dst_finished;
    unsigned         nb_dst;

    // [0] - index of the sync queue in Scheduler.sq_enc,
    // [1] - index of this encoder in the sq
    int                 sq_idx[2];

    /* Opening encoders is somewhat nontrivial due to their interaction with
     * sync queues, which are (among other things) responsible for maintaining
     * constant audio frame size, when it is required by the encoder.
     *
     * Opening the encoder requires stream parameters, obtained from the first
     * frame. However, that frame cannot be properly chunked by the sync queue
     * without knowing the required frame size, which is only available after
     * opening the encoder.
     *
     * This apparent circular dependency is resolved in the following way:
     * - the caller creating the encoder gives us a callback which opens the
     *   encoder and returns the required frame size (if any)
     * - when the first frame is sent to the encoder, the sending thread
     *      - calls this callback, opening the encoder
     *      - passes the returned frame size to the sync queue
     */
    int               (*open_cb)(void *opaque, const AVFrame *frame);
    int                 opened;

    SchTask             task;
    // Queue for receiving input frames, one stream.
    ThreadQueue        *queue;
    // tq_send() to queue returned EOF
    int                 in_finished;

    // temporary storage used by sch_enc_send()
    AVPacket           *send_pkt;
} SchEnc;

typedef struct SchDemuxStream {
    SchedulerNode      *dst;
    uint8_t            *dst_finished;
    unsigned         nb_dst;
} SchDemuxStream;

typedef struct SchDemux {
    const AVClass      *class;

    SchDemuxStream     *streams;
    unsigned         nb_streams;

    SchTask             task;
    SchWaiter           waiter;

    // temporary storage used by sch_demux_send()
    AVPacket           *send_pkt;

    // protected by schedule_lock
    int                 task_exited;
} SchDemux;

typedef struct PreMuxQueue {
    /**
     * Queue for buffering the packets before the muxer task can be started.
     */
    AVFifo         *fifo;
    /**
     * Maximum number of packets in fifo.
     */
    int             max_packets;
    /*
     * The size of the AVPackets' buffers in queue.
     * Updated when a packet is either pushed or pulled from the queue.
     */
    size_t          data_size;
    /* Threshold after which max_packets will be in effect */
    size_t          data_threshold;
} PreMuxQueue;

typedef struct SchMuxStream {
    SchedulerNode       src;

    unsigned           *sub_heartbeat_dst;
    unsigned         nb_sub_heartbeat_dst;

    PreMuxQueue         pre_mux_queue;

    // an EOF was generated while flushing the pre-mux queue
    int                 init_eof;

    ////////////////////////////////////////////////////////////
    // The following are protected by Scheduler.schedule_lock //

    /* dts+duration of the last packet sent to this stream
       in AV_TIME_BASE_Q */
    int64_t             last_dts;
    // this stream no longer accepts input
    int                 source_finished;
    ////////////////////////////////////////////////////////////
} SchMuxStream;

typedef struct SchMux {
    const AVClass      *class;

    SchMuxStream       *streams;
    unsigned         nb_streams;
    unsigned         nb_streams_ready;

    int               (*init)(void *arg);

    SchTask             task;
    /**
     * Set to 1 after starting the muxer task and flushing the
     * pre-muxing queues.
     * Set either before any tasks have started, or with
     * Scheduler.mux_ready_lock held.
     */
    atomic_int          mux_started;
    ThreadQueue        *queue;
    unsigned            queue_size;

    AVPacket           *sub_heartbeat_pkt;
} SchMux;

typedef struct SchFilterIn {
    SchedulerNode       src;
    int                 send_finished;
    int                 receive_finished;
} SchFilterIn;

typedef struct SchFilterOut {
    SchedulerNode       dst;
} SchFilterOut;

typedef struct SchFilterGraph {
    const AVClass      *class;

    SchFilterIn        *inputs;
    unsigned         nb_inputs;
    unsigned         nb_inputs_finished_send;
    unsigned         nb_inputs_finished_receive;

    SchFilterOut       *outputs;
    unsigned         nb_outputs;

    SchTask             task;
    // input queue, nb_inputs+1 streams
    // last stream is control
    ThreadQueue        *queue;
    SchWaiter           waiter;

    // protected by schedule_lock
    unsigned            best_input;
    int                 task_exited;
} SchFilterGraph;

enum SchedulerState {
    SCH_STATE_UNINIT,
    SCH_STATE_STARTED,
    SCH_STATE_STOPPED,
};

struct Scheduler {
    const AVClass      *class;

    SchDemux           *demux;
    unsigned         nb_demux;

    SchMux             *mux;
    unsigned         nb_mux;

    unsigned         nb_mux_ready;
    pthread_mutex_t     mux_ready_lock;

    unsigned         nb_mux_done;
    unsigned            task_failed;
    pthread_mutex_t     finish_lock;
    pthread_cond_t      finish_cond;


    SchDec             *dec;
    unsigned         nb_dec;

    SchEnc             *enc;
    unsigned         nb_enc;

    SchSyncQueue       *sq_enc;
    unsigned         nb_sq_enc;

    SchFilterGraph     *filters;
    unsigned         nb_filters;

    char               *sdp_filename;
    int                 sdp_auto;

    enum SchedulerState state;
    atomic_int          terminate;

    pthread_mutex_t     schedule_lock;

    atomic_int_least64_t last_dts;
};

/**
 * Wait until this task is allowed to proceed.
 *
 * @retval 0 the caller should proceed
 * @retval 1 the caller should terminate
 */
static int waiter_wait(Scheduler *sch, SchWaiter *w)
{
    int terminate;

    if (!atomic_load(&w->choked))
        return 0;

    pthread_mutex_lock(&w->lock);

    while (atomic_load(&w->choked) && !atomic_load(&sch->terminate))
        pthread_cond_wait(&w->cond, &w->lock);

    terminate = atomic_load(&sch->terminate);

    pthread_mutex_unlock(&w->lock);

    return terminate;
}

static void waiter_set(SchWaiter *w, int choked)
{
    pthread_mutex_lock(&w->lock);

    atomic_store(&w->choked, choked);
    pthread_cond_signal(&w->cond);

    pthread_mutex_unlock(&w->lock);
}

static int waiter_init(SchWaiter *w)
{
    int ret;

    atomic_init(&w->choked, 0);

    ret = pthread_mutex_init(&w->lock, NULL);
    if (ret)
        return AVERROR(ret);

    ret = pthread_cond_init(&w->cond, NULL);
    if (ret)
        return AVERROR(ret);

    return 0;
}

static void waiter_uninit(SchWaiter *w)
{
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
}

static int queue_alloc(ThreadQueue **ptq, unsigned nb_streams, unsigned queue_size,
                       enum QueueType type)
{
    ThreadQueue *tq;

    if (queue_size <= 0) {
        if (type == QUEUE_FRAMES)
            queue_size = DEFAULT_FRAME_THREAD_QUEUE_SIZE;
        else
            queue_size = DEFAULT_PACKET_THREAD_QUEUE_SIZE;
    }

    if (type == QUEUE_FRAMES) {
        // This queue length is used in the decoder code to ensure that
        // there are enough entries in fixed-size frame pools to account
        // for frames held in queues inside the ffmpeg utility.  If this
        // can ever dynamically change then the corresponding decode
        // code needs to be updated as well.
        av_assert0(queue_size <= DEFAULT_FRAME_THREAD_QUEUE_SIZE);
    }

    tq = tq_alloc(nb_streams, queue_size,
                  (type == QUEUE_PACKETS) ? THREAD_QUEUE_PACKETS : THREAD_QUEUE_FRAMES);
    if (!tq)
        return AVERROR(ENOMEM);

    *ptq = tq;
    return 0;
}

static void *task_wrapper(void *arg);

static int task_start(SchTask *task)
{
    int ret;

    if (!task->parent)
        return 0;

    av_log(task->func_arg, AV_LOG_VERBOSE, "Starting thread...\n");

    av_assert0(!task->thread_running);

    ret = pthread_create(&task->thread, NULL, task_wrapper, task);
    if (ret) {
        av_log(task->func_arg, AV_LOG_ERROR, "pthread_create() failed: %s\n",
               strerror(ret));
        return AVERROR(ret);
    }

    task->thread_running = 1;
    return 0;
}

static void task_init(Scheduler *sch, SchTask *task, enum SchedulerNodeType type, unsigned idx,
                      SchThreadFunc func, void *func_arg)
{
    task->parent    = sch;

    task->node.type = type;
    task->node.idx  = idx;

    task->func      = func;
    task->func_arg  = func_arg;
}

static int64_t trailing_dts(const Scheduler *sch, int count_finished)
{
    int64_t min_dts = INT64_MAX;

    for (unsigned i = 0; i < sch->nb_mux; i++) {
        const SchMux *mux = &sch->mux[i];

        for (unsigned j = 0; j < mux->nb_streams; j++) {
            const SchMuxStream *ms = &mux->streams[j];

            if (ms->source_finished && !count_finished)
                continue;
            if (ms->last_dts == AV_NOPTS_VALUE)
                return AV_NOPTS_VALUE;

            min_dts = FFMIN(min_dts, ms->last_dts);
        }
    }

    return min_dts == INT64_MAX ? AV_NOPTS_VALUE : min_dts;
}

void sch_remove_filtergraph(Scheduler *sch, int idx)
{
    SchFilterGraph *fg = &sch->filters[idx];

    av_assert0(!fg->task.thread_running);
    memset(&fg->task, 0, sizeof(fg->task));

    tq_free(&fg->queue);

    av_freep(&fg->inputs);
    fg->nb_inputs = 0;
    av_freep(&fg->outputs);
    fg->nb_outputs = 0;

    fg->task_exited = 1;
}

void sch_free(Scheduler **psch)
{
    Scheduler *sch = *psch;

    if (!sch)
        return;

    sch_stop(sch, NULL);

    for (unsigned i = 0; i < sch->nb_demux; i++) {
        SchDemux *d = &sch->demux[i];

        for (unsigned j = 0; j < d->nb_streams; j++) {
            SchDemuxStream *ds = &d->streams[j];
            av_freep(&ds->dst);
            av_freep(&ds->dst_finished);
        }
        av_freep(&d->streams);

        av_packet_free(&d->send_pkt);

        waiter_uninit(&d->waiter);
    }
    av_freep(&sch->demux);

    for (unsigned i = 0; i < sch->nb_mux; i++) {
        SchMux *mux = &sch->mux[i];

        for (unsigned j = 0; j < mux->nb_streams; j++) {
            SchMuxStream *ms = &mux->streams[j];

            if (ms->pre_mux_queue.fifo) {
                AVPacket *pkt;
                while (av_fifo_read(ms->pre_mux_queue.fifo, &pkt, 1) >= 0)
                    av_packet_free(&pkt);
                av_fifo_freep2(&ms->pre_mux_queue.fifo);
            }

            av_freep(&ms->sub_heartbeat_dst);
        }
        av_freep(&mux->streams);

        av_packet_free(&mux->sub_heartbeat_pkt);

        tq_free(&mux->queue);
    }
    av_freep(&sch->mux);

    for (unsigned i = 0; i < sch->nb_dec; i++) {
        SchDec *dec = &sch->dec[i];

        tq_free(&dec->queue);

        av_thread_message_queue_free(&dec->queue_end_ts);

        for (unsigned j = 0; j < dec->nb_outputs; j++) {
            SchDecOutput *o = &dec->outputs[j];

            av_freep(&o->dst);
            av_freep(&o->dst_finished);
        }

        av_freep(&dec->outputs);

        av_frame_free(&dec->send_frame);
    }
    av_freep(&sch->dec);

    for (unsigned i = 0; i < sch->nb_enc; i++) {
        SchEnc *enc = &sch->enc[i];

        tq_free(&enc->queue);

        av_packet_free(&enc->send_pkt);

        av_freep(&enc->dst);
        av_freep(&enc->dst_finished);
    }
    av_freep(&sch->enc);

    for (unsigned i = 0; i < sch->nb_sq_enc; i++) {
        SchSyncQueue *sq = &sch->sq_enc[i];
        sq_free(&sq->sq);
        av_frame_free(&sq->frame);
        pthread_mutex_destroy(&sq->lock);
        av_freep(&sq->enc_idx);
    }
    av_freep(&sch->sq_enc);

    for (unsigned i = 0; i < sch->nb_filters; i++) {
        SchFilterGraph *fg = &sch->filters[i];

        tq_free(&fg->queue);

        av_freep(&fg->inputs);
        av_freep(&fg->outputs);

        waiter_uninit(&fg->waiter);
    }
    av_freep(&sch->filters);

    av_freep(&sch->sdp_filename);

    pthread_mutex_destroy(&sch->schedule_lock);

    pthread_mutex_destroy(&sch->mux_ready_lock);

    pthread_mutex_destroy(&sch->finish_lock);
    pthread_cond_destroy(&sch->finish_cond);

    av_freep(psch);
}

static const AVClass scheduler_class = {
    .class_name = "Scheduler",
    .version    = LIBAVUTIL_VERSION_INT,
};

Scheduler *sch_alloc(void)
{
    Scheduler *sch;
    int ret;

    sch = av_mallocz(sizeof(*sch));
    if (!sch)
        return NULL;

    sch->class    = &scheduler_class;
    sch->sdp_auto = 1;

    ret = pthread_mutex_init(&sch->schedule_lock, NULL);
    if (ret)
        goto fail;

    ret = pthread_mutex_init(&sch->mux_ready_lock, NULL);
    if (ret)
        goto fail;

    ret = pthread_mutex_init(&sch->finish_lock, NULL);
    if (ret)
        goto fail;

    ret = pthread_cond_init(&sch->finish_cond, NULL);
    if (ret)
        goto fail;

    return sch;
fail:
    sch_free(&sch);
    return NULL;
}

int sch_sdp_filename(Scheduler *sch, const char *sdp_filename)
{
    av_freep(&sch->sdp_filename);
    sch->sdp_filename = av_strdup(sdp_filename);
    return sch->sdp_filename ? 0 : AVERROR(ENOMEM);
}

static const AVClass sch_mux_class = {
    .class_name                = "SchMux",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(SchMux, task.func_arg),
};

int sch_add_mux(Scheduler *sch, SchThreadFunc func, int (*init)(void *),
                void *arg, int sdp_auto, unsigned thread_queue_size)
{
    const unsigned idx = sch->nb_mux;

    SchMux *mux;
    int ret;

    ret = GROW_ARRAY(sch->mux, sch->nb_mux);
    if (ret < 0)
        return ret;

    mux             = &sch->mux[idx];
    mux->class      = &sch_mux_class;
    mux->init       = init;
    mux->queue_size = thread_queue_size;

    task_init(sch, &mux->task, SCH_NODE_TYPE_MUX, idx, func, arg);

    sch->sdp_auto &= sdp_auto;

    return idx;
}

int sch_add_mux_stream(Scheduler *sch, unsigned mux_idx)
{
    SchMux       *mux;
    SchMuxStream *ms;
    unsigned      stream_idx;
    int ret;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    ret = GROW_ARRAY(mux->streams, mux->nb_streams);
    if (ret < 0)
        return ret;
    stream_idx = mux->nb_streams - 1;

    ms = &mux->streams[stream_idx];

    ms->pre_mux_queue.fifo = av_fifo_alloc2(8, sizeof(AVPacket*), 0);
    if (!ms->pre_mux_queue.fifo)
        return AVERROR(ENOMEM);

    ms->last_dts = AV_NOPTS_VALUE;

    return stream_idx;
}

static const AVClass sch_demux_class = {
    .class_name                = "SchDemux",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(SchDemux, task.func_arg),
};

int sch_add_demux(Scheduler *sch, SchThreadFunc func, void *ctx)
{
    const unsigned idx = sch->nb_demux;

    SchDemux *d;
    int ret;

    ret = GROW_ARRAY(sch->demux, sch->nb_demux);
    if (ret < 0)
        return ret;

    d = &sch->demux[idx];

    task_init(sch, &d->task, SCH_NODE_TYPE_DEMUX, idx, func, ctx);

    d->class    = &sch_demux_class;
    d->send_pkt = av_packet_alloc();
    if (!d->send_pkt)
        return AVERROR(ENOMEM);

    ret = waiter_init(&d->waiter);
    if (ret < 0)
        return ret;

    return idx;
}

int sch_add_demux_stream(Scheduler *sch, unsigned demux_idx)
{
    SchDemux *d;
    int ret;

    av_assert0(demux_idx < sch->nb_demux);
    d = &sch->demux[demux_idx];

    ret = GROW_ARRAY(d->streams, d->nb_streams);
    return ret < 0 ? ret : d->nb_streams - 1;
}

int sch_add_dec_output(Scheduler *sch, unsigned dec_idx)
{
    SchDec *dec;
    int ret;

    av_assert0(dec_idx < sch->nb_dec);
    dec = &sch->dec[dec_idx];

    ret = GROW_ARRAY(dec->outputs, dec->nb_outputs);
    if (ret < 0)
        return ret;

    return dec->nb_outputs - 1;
}

static const AVClass sch_dec_class = {
    .class_name                = "SchDec",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(SchDec, task.func_arg),
};

int sch_add_dec(Scheduler *sch, SchThreadFunc func, void *ctx, int send_end_ts)
{
    const unsigned idx = sch->nb_dec;

    SchDec *dec;
    int ret;

    ret = GROW_ARRAY(sch->dec, sch->nb_dec);
    if (ret < 0)
        return ret;

    dec = &sch->dec[idx];

    task_init(sch, &dec->task, SCH_NODE_TYPE_DEC, idx, func, ctx);

    dec->class      = &sch_dec_class;
    dec->send_frame = av_frame_alloc();
    if (!dec->send_frame)
        return AVERROR(ENOMEM);

    ret = sch_add_dec_output(sch, idx);
    if (ret < 0)
        return ret;

    ret = queue_alloc(&dec->queue, 1, 0, QUEUE_PACKETS);
    if (ret < 0)
        return ret;

    if (send_end_ts) {
        ret = av_thread_message_queue_alloc(&dec->queue_end_ts, 1, sizeof(Timestamp));
        if (ret < 0)
            return ret;
    }

    return idx;
}

static const AVClass sch_enc_class = {
    .class_name                = "SchEnc",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(SchEnc, task.func_arg),
};

int sch_add_enc(Scheduler *sch, SchThreadFunc func, void *ctx,
                int (*open_cb)(void *opaque, const AVFrame *frame))
{
    const unsigned idx = sch->nb_enc;

    SchEnc *enc;
    int ret;

    ret = GROW_ARRAY(sch->enc, sch->nb_enc);
    if (ret < 0)
        return ret;

    enc             = &sch->enc[idx];

    enc->class      = &sch_enc_class;
    enc->open_cb    = open_cb;
    enc->sq_idx[0]  = -1;
    enc->sq_idx[1]  = -1;

    task_init(sch, &enc->task, SCH_NODE_TYPE_ENC, idx, func, ctx);

    enc->send_pkt = av_packet_alloc();
    if (!enc->send_pkt)
        return AVERROR(ENOMEM);

    ret = queue_alloc(&enc->queue, 1, 0, QUEUE_FRAMES);
    if (ret < 0)
        return ret;

    return idx;
}

static const AVClass sch_fg_class = {
    .class_name                = "SchFilterGraph",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(SchFilterGraph, task.func_arg),
};

int sch_add_filtergraph(Scheduler *sch, unsigned nb_inputs, unsigned nb_outputs,
                        SchThreadFunc func, void *ctx)
{
    const unsigned idx = sch->nb_filters;

    SchFilterGraph *fg;
    int ret;

    ret = GROW_ARRAY(sch->filters, sch->nb_filters);
    if (ret < 0)
        return ret;
    fg = &sch->filters[idx];

    fg->class = &sch_fg_class;

    task_init(sch, &fg->task, SCH_NODE_TYPE_FILTER_IN, idx, func, ctx);

    if (nb_inputs) {
        fg->inputs = av_calloc(nb_inputs, sizeof(*fg->inputs));
        if (!fg->inputs)
            return AVERROR(ENOMEM);
        fg->nb_inputs = nb_inputs;
    }

    if (nb_outputs) {
        fg->outputs = av_calloc(nb_outputs, sizeof(*fg->outputs));
        if (!fg->outputs)
            return AVERROR(ENOMEM);
        fg->nb_outputs = nb_outputs;
    }

    ret = waiter_init(&fg->waiter);
    if (ret < 0)
        return ret;

    ret = queue_alloc(&fg->queue, fg->nb_inputs + 1, 0, QUEUE_FRAMES);
    if (ret < 0)
        return ret;

    return idx;
}

int sch_add_sq_enc(Scheduler *sch, uint64_t buf_size_us, void *logctx)
{
    SchSyncQueue *sq;
    int ret;

    ret = GROW_ARRAY(sch->sq_enc, sch->nb_sq_enc);
    if (ret < 0)
        return ret;
    sq = &sch->sq_enc[sch->nb_sq_enc - 1];

    sq->sq = sq_alloc(SYNC_QUEUE_FRAMES, buf_size_us, logctx);
    if (!sq->sq)
        return AVERROR(ENOMEM);

    sq->frame = av_frame_alloc();
    if (!sq->frame)
        return AVERROR(ENOMEM);

    ret = pthread_mutex_init(&sq->lock, NULL);
    if (ret)
        return AVERROR(ret);

    return sq - sch->sq_enc;
}

int sch_sq_add_enc(Scheduler *sch, unsigned sq_idx, unsigned enc_idx,
                   int limiting, uint64_t max_frames)
{
    SchSyncQueue *sq;
    SchEnc *enc;
    int ret;

    av_assert0(sq_idx < sch->nb_sq_enc);
    sq = &sch->sq_enc[sq_idx];

    av_assert0(enc_idx < sch->nb_enc);
    enc = &sch->enc[enc_idx];

    ret = GROW_ARRAY(sq->enc_idx, sq->nb_enc_idx);
    if (ret < 0)
        return ret;
    sq->enc_idx[sq->nb_enc_idx - 1] = enc_idx;

    ret = sq_add_stream(sq->sq, limiting);
    if (ret < 0)
        return ret;

    enc->sq_idx[0] = sq_idx;
    enc->sq_idx[1] = ret;

    if (max_frames != INT64_MAX)
        sq_limit_frames(sq->sq, enc->sq_idx[1], max_frames);

    return 0;
}

int sch_connect(Scheduler *sch, SchedulerNode src, SchedulerNode dst)
{
    int ret;

    switch (src.type) {
    case SCH_NODE_TYPE_DEMUX: {
        SchDemuxStream *ds;

        av_assert0(src.idx < sch->nb_demux &&
                   src.idx_stream < sch->demux[src.idx].nb_streams);
        ds = &sch->demux[src.idx].streams[src.idx_stream];

        ret = GROW_ARRAY(ds->dst, ds->nb_dst);
        if (ret < 0)
            return ret;

        ds->dst[ds->nb_dst - 1] = dst;

        // demuxed packets go to decoding or streamcopy
        switch (dst.type) {
        case SCH_NODE_TYPE_DEC: {
            SchDec *dec;

            av_assert0(dst.idx < sch->nb_dec);
            dec = &sch->dec[dst.idx];

            av_assert0(!dec->src.type);
            dec->src = src;
            break;
            }
        case SCH_NODE_TYPE_MUX: {
            SchMuxStream *ms;

            av_assert0(dst.idx < sch->nb_mux &&
                       dst.idx_stream < sch->mux[dst.idx].nb_streams);
            ms = &sch->mux[dst.idx].streams[dst.idx_stream];

            av_assert0(!ms->src.type);
            ms->src = src;

            break;
            }
        default: av_assert0(0);
        }

        break;
        }
    case SCH_NODE_TYPE_DEC: {
        SchDec *dec;
        SchDecOutput *o;

        av_assert0(src.idx < sch->nb_dec);
        dec = &sch->dec[src.idx];

        av_assert0(src.idx_stream < dec->nb_outputs);
        o = &dec->outputs[src.idx_stream];

        ret = GROW_ARRAY(o->dst, o->nb_dst);
        if (ret < 0)
            return ret;

        o->dst[o->nb_dst - 1] = dst;

        // decoded frames go to filters or encoding
        switch (dst.type) {
        case SCH_NODE_TYPE_FILTER_IN: {
            SchFilterIn *fi;

            av_assert0(dst.idx < sch->nb_filters &&
                       dst.idx_stream < sch->filters[dst.idx].nb_inputs);
            fi = &sch->filters[dst.idx].inputs[dst.idx_stream];

            av_assert0(!fi->src.type);
            fi->src = src;
            break;
            }
        case SCH_NODE_TYPE_ENC: {
            SchEnc *enc;

            av_assert0(dst.idx < sch->nb_enc);
            enc = &sch->enc[dst.idx];

            av_assert0(!enc->src.type);
            enc->src = src;
            break;
            }
        default: av_assert0(0);
        }

        break;
        }
    case SCH_NODE_TYPE_FILTER_OUT: {
        SchFilterOut *fo;

        av_assert0(src.idx < sch->nb_filters &&
                   src.idx_stream < sch->filters[src.idx].nb_outputs);
        fo = &sch->filters[src.idx].outputs[src.idx_stream];

        av_assert0(!fo->dst.type);
        fo->dst = dst;

        // filtered frames go to encoding or another filtergraph
        switch (dst.type) {
        case SCH_NODE_TYPE_ENC: {
            SchEnc *enc;

            av_assert0(dst.idx < sch->nb_enc);
            enc = &sch->enc[dst.idx];

            av_assert0(!enc->src.type);
            enc->src = src;
            break;
            }
        case SCH_NODE_TYPE_FILTER_IN: {
            SchFilterIn *fi;

            av_assert0(dst.idx < sch->nb_filters &&
                       dst.idx_stream < sch->filters[dst.idx].nb_inputs);
            fi = &sch->filters[dst.idx].inputs[dst.idx_stream];

            av_assert0(!fi->src.type);
            fi->src = src;
            break;
            }
        default: av_assert0(0);
        }


        break;
        }
    case SCH_NODE_TYPE_ENC: {
        SchEnc       *enc;

        av_assert0(src.idx < sch->nb_enc);
        enc = &sch->enc[src.idx];

        ret = GROW_ARRAY(enc->dst, enc->nb_dst);
        if (ret < 0)
            return ret;

        enc->dst[enc->nb_dst - 1] = dst;

        // encoding packets go to muxing or decoding
        switch (dst.type) {
        case SCH_NODE_TYPE_MUX: {
            SchMuxStream *ms;

            av_assert0(dst.idx        < sch->nb_mux &&
                       dst.idx_stream < sch->mux[dst.idx].nb_streams);
            ms = &sch->mux[dst.idx].streams[dst.idx_stream];

            av_assert0(!ms->src.type);
            ms->src  = src;

            break;
            }
        case SCH_NODE_TYPE_DEC: {
            SchDec *dec;

            av_assert0(dst.idx < sch->nb_dec);
            dec = &sch->dec[dst.idx];

            av_assert0(!dec->src.type);
            dec->src = src;

            break;
            }
        default: av_assert0(0);
        }

        break;
        }
    default: av_assert0(0);
    }

    return 0;
}

static int mux_task_start(SchMux *mux)
{
    int ret = 0;

    ret = task_start(&mux->task);
    if (ret < 0)
        return ret;

    /* flush the pre-muxing queues */
    while (1) {
        int       min_stream = -1;
        Timestamp min_ts     = { .ts = AV_NOPTS_VALUE };

        AVPacket *pkt;

        // find the stream with the earliest dts or EOF in pre-muxing queue
        for (unsigned i = 0; i < mux->nb_streams; i++) {
            SchMuxStream *ms = &mux->streams[i];

            if (av_fifo_peek(ms->pre_mux_queue.fifo, &pkt, 1, 0) < 0)
                continue;

            if (!pkt || pkt->dts == AV_NOPTS_VALUE) {
                min_stream = i;
                break;
            }

            if (min_ts.ts == AV_NOPTS_VALUE ||
                av_compare_ts(min_ts.ts, min_ts.tb, pkt->dts, pkt->time_base) > 0) {
                min_stream = i;
                min_ts     = (Timestamp){ .ts = pkt->dts, .tb = pkt->time_base };
            }
        }

        if (min_stream >= 0) {
            SchMuxStream *ms = &mux->streams[min_stream];

            ret = av_fifo_read(ms->pre_mux_queue.fifo, &pkt, 1);
            av_assert0(ret >= 0);

            if (pkt) {
                if (!ms->init_eof)
                    ret = tq_send(mux->queue, min_stream, pkt);
                av_packet_free(&pkt);
                if (ret == AVERROR_EOF)
                    ms->init_eof = 1;
                else if (ret < 0)
                    return ret;
            } else
                tq_send_finish(mux->queue, min_stream);

            continue;
        }

        break;
    }

    atomic_store(&mux->mux_started, 1);

    return 0;
}

int print_sdp(const char *filename);

static int mux_init(Scheduler *sch, SchMux *mux)
{
    int ret;

    ret = mux->init(mux->task.func_arg);
    if (ret < 0)
        return ret;

    sch->nb_mux_ready++;

    if (sch->sdp_filename || sch->sdp_auto) {
        if (sch->nb_mux_ready < sch->nb_mux)
            return 0;

        ret = print_sdp(sch->sdp_filename);
        if (ret < 0) {
            av_log(sch, AV_LOG_ERROR, "Error writing the SDP.\n");
            return ret;
        }

        /* SDP is written only after all the muxers are ready, so now we
         * start ALL the threads */
        for (unsigned i = 0; i < sch->nb_mux; i++) {
            ret = mux_task_start(&sch->mux[i]);
            if (ret < 0)
                return ret;
        }
    } else {
        ret = mux_task_start(mux);
        if (ret < 0)
            return ret;
    }

    return 0;
}

void sch_mux_stream_buffering(Scheduler *sch, unsigned mux_idx, unsigned stream_idx,
                              size_t data_threshold, int max_packets)
{
    SchMux       *mux;
    SchMuxStream *ms;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    av_assert0(stream_idx < mux->nb_streams);
    ms = &mux->streams[stream_idx];

    ms->pre_mux_queue.max_packets    = max_packets;
    ms->pre_mux_queue.data_threshold = data_threshold;
}

int sch_mux_stream_ready(Scheduler *sch, unsigned mux_idx, unsigned stream_idx)
{
    SchMux *mux;
    int ret = 0;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    av_assert0(stream_idx < mux->nb_streams);

    pthread_mutex_lock(&sch->mux_ready_lock);

    av_assert0(mux->nb_streams_ready < mux->nb_streams);

    // this may be called during initialization - do not start
    // threads before sch_start() is called
    if (++mux->nb_streams_ready == mux->nb_streams &&
        sch->state >= SCH_STATE_STARTED)
        ret = mux_init(sch, mux);

    pthread_mutex_unlock(&sch->mux_ready_lock);

    return ret;
}

int sch_mux_sub_heartbeat_add(Scheduler *sch, unsigned mux_idx, unsigned stream_idx,
                              unsigned dec_idx)
{
    SchMux       *mux;
    SchMuxStream *ms;
    int ret = 0;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    av_assert0(stream_idx < mux->nb_streams);
    ms = &mux->streams[stream_idx];

    ret = GROW_ARRAY(ms->sub_heartbeat_dst, ms->nb_sub_heartbeat_dst);
    if (ret < 0)
        return ret;

    av_assert0(dec_idx < sch->nb_dec);
    ms->sub_heartbeat_dst[ms->nb_sub_heartbeat_dst - 1] = dec_idx;

    if (!mux->sub_heartbeat_pkt) {
        mux->sub_heartbeat_pkt = av_packet_alloc();
        if (!mux->sub_heartbeat_pkt)
            return AVERROR(ENOMEM);
    }

    return 0;
}

static void unchoke_for_stream(Scheduler *sch, SchedulerNode src);

// Unchoke any filter graphs that are downstream of this node, to prevent it
// from getting stuck trying to push data to a full queue
static void unchoke_downstream(Scheduler *sch, SchedulerNode *dst)
{
    SchFilterGraph *fg;
    SchDec *dec;
    SchEnc *enc;
    switch (dst->type) {
    case SCH_NODE_TYPE_DEC:
        dec = &sch->dec[dst->idx];
        for (int i = 0; i < dec->nb_outputs; i++)
            unchoke_downstream(sch, dec->outputs[i].dst);
        break;
    case SCH_NODE_TYPE_ENC:
        enc = &sch->enc[dst->idx];
        for (int i = 0; i < enc->nb_dst; i++)
            unchoke_downstream(sch, &enc->dst[i]);
        break;
    case SCH_NODE_TYPE_MUX:
        // muxers are never choked
        break;
    case SCH_NODE_TYPE_FILTER_IN:
        fg = &sch->filters[dst->idx];
        if (fg->best_input == fg->nb_inputs) {
            fg->waiter.choked_next = 0;
        } else {
            // ensure that this filter graph is not stuck waiting for
            // input from a different upstream demuxer
            unchoke_for_stream(sch, fg->inputs[fg->best_input].src);
        }
        break;
    default:
        av_unreachable("Invalid destination node type?");
        break;
    }
}

static void unchoke_for_stream(Scheduler *sch, SchedulerNode src)
{
    while (1) {
        SchFilterGraph *fg;
        SchDemux *demux;
        switch (src.type) {
        case SCH_NODE_TYPE_DEMUX:
            // fed directly by a demuxer (i.e. not through a filtergraph)
            demux = &sch->demux[src.idx];
            if (demux->waiter.choked_next == 0)
                return; // prevent infinite loop
            demux->waiter.choked_next = 0;
            for (int i = 0; i < demux->nb_streams; i++)
                unchoke_downstream(sch, demux->streams[i].dst);
            return;
        case SCH_NODE_TYPE_DEC:
            src = sch->dec[src.idx].src;
            continue;
        case SCH_NODE_TYPE_ENC:
            src = sch->enc[src.idx].src;
            continue;
        case SCH_NODE_TYPE_FILTER_OUT:
            fg = &sch->filters[src.idx];
            // the filtergraph contains internal sources and
            // requested to be scheduled directly
            if (fg->best_input == fg->nb_inputs) {
                fg->waiter.choked_next = 0;
                return;
            }
            src = fg->inputs[fg->best_input].src;
            continue;
        default:
            av_unreachable("Invalid source node type?");
            return;
        }
    }
}

static void choke_demux(const Scheduler *sch, int demux_id, int choked)
{
    av_assert1(demux_id < sch->nb_demux);
    SchDemux *demux = &sch->demux[demux_id];

    for (int i = 0; i < demux->nb_streams; i++) {
        SchedulerNode *dst = demux->streams[i].dst;
        SchFilterGraph *fg;

        switch (dst->type) {
        case SCH_NODE_TYPE_DEC:
            tq_choke(sch->dec[dst->idx].queue, choked);
            break;
        case SCH_NODE_TYPE_ENC:
            tq_choke(sch->enc[dst->idx].queue, choked);
            break;
        case SCH_NODE_TYPE_MUX:
            break;
        case SCH_NODE_TYPE_FILTER_IN:
            fg = &sch->filters[dst->idx];
            if (fg->nb_inputs == 1)
                tq_choke(fg->queue, choked);
            break;
        default:
            av_unreachable("Invalid destination node type?");
            break;
        }
    }
}

static void schedule_update_locked(Scheduler *sch)
{
    int64_t dts;
    int have_unchoked = 0;

    // on termination request all waiters are choked,
    // we are not to unchoke them
    if (atomic_load(&sch->terminate))
        return;

    dts = trailing_dts(sch, 0);

    atomic_store(&sch->last_dts, dts);

    // initialize our internal state
    for (unsigned type = 0; type < 2; type++)
        for (unsigned i = 0; i < (type ? sch->nb_filters : sch->nb_demux); i++) {
            SchWaiter *w = type ? &sch->filters[i].waiter : &sch->demux[i].waiter;
            w->choked_prev = atomic_load(&w->choked);
            w->choked_next = 1;
        }

    // figure out the sources that are allowed to proceed
    for (unsigned i = 0; i < sch->nb_mux; i++) {
        SchMux *mux = &sch->mux[i];

        for (unsigned j = 0; j < mux->nb_streams; j++) {
            SchMuxStream *ms = &mux->streams[j];

            // unblock sources for output streams that are not finished
            // and not too far ahead of the trailing stream
            if (ms->source_finished)
                continue;
            if (dts == AV_NOPTS_VALUE && ms->last_dts != AV_NOPTS_VALUE)
                continue;
            if (dts != AV_NOPTS_VALUE && ms->last_dts - dts >= SCHEDULE_TOLERANCE)
                continue;

            // resolve the source to unchoke
            unchoke_for_stream(sch, ms->src);
            have_unchoked = 1;
        }
    }

    // also unchoke any sources feeding into closed filter graph inputs, so
    // that they can observe the downstream EOF
    for (unsigned i = 0; i < sch->nb_filters; i++) {
        SchFilterGraph *fg = &sch->filters[i];

        for (unsigned j = 0; j < fg->nb_inputs; j++) {
            SchFilterIn *fi = &fg->inputs[j];
            if (fi->receive_finished && !fi->send_finished)
                unchoke_for_stream(sch, fi->src);
        }
    }

    // make sure to unchoke at least one source, if still available
    for (unsigned type = 0; !have_unchoked && type < 2; type++)
        for (unsigned i = 0; i < (type ? sch->nb_filters : sch->nb_demux); i++) {
            int exited = type ? sch->filters[i].task_exited : sch->demux[i].task_exited;
            SchWaiter *w = type ? &sch->filters[i].waiter : &sch->demux[i].waiter;
            if (!exited) {
                w->choked_next = 0;
                have_unchoked  = 1;
                break;
            }
        }

    for (unsigned type = 0; type < 2; type++) {
        for (unsigned i = 0; i < (type ? sch->nb_filters : sch->nb_demux); i++) {
            SchWaiter *w = type ? &sch->filters[i].waiter : &sch->demux[i].waiter;
            if (w->choked_prev != w->choked_next) {
                waiter_set(w, w->choked_next);
                if (!type)
                    choke_demux(sch, i, w->choked_next);
            }
        }
    }

}

enum {
    CYCLE_NODE_NEW = 0,
    CYCLE_NODE_STARTED,
    CYCLE_NODE_DONE,
};

// Finds the filtergraph or muxer upstream of a scheduler node
static SchedulerNode src_filtergraph(const Scheduler *sch, SchedulerNode src)
{
    while (1) {
        switch (src.type) {
        case SCH_NODE_TYPE_DEMUX:
        case SCH_NODE_TYPE_FILTER_OUT:
            return src;
        case SCH_NODE_TYPE_DEC:
            src = sch->dec[src.idx].src;
            continue;
        case SCH_NODE_TYPE_ENC:
            src = sch->enc[src.idx].src;
            continue;
        default:
            av_unreachable("Invalid source node type?");
            return (SchedulerNode) {0};
        }
    }
}

static int
check_acyclic_for_output(const Scheduler *sch, SchedulerNode src,
                         uint8_t *filters_visited, SchedulerNode *filters_stack)
{
    unsigned nb_filters_stack = 0;

    memset(filters_visited, 0, sch->nb_filters * sizeof(*filters_visited));

    while (1) {
        const SchFilterGraph *fg = &sch->filters[src.idx];

        filters_visited[src.idx] = CYCLE_NODE_STARTED;

        // descend into every input, depth first
        if (src.idx_stream < fg->nb_inputs) {
            const SchFilterIn *fi = &fg->inputs[src.idx_stream++];
            SchedulerNode node = src_filtergraph(sch, fi->src);

            // connected to demuxer, no cycles possible
            if (node.type == SCH_NODE_TYPE_DEMUX)
                continue;

            // otherwise connected to another filtergraph
            av_assert0(node.type == SCH_NODE_TYPE_FILTER_OUT);

            // found a cycle
            if (filters_visited[node.idx] == CYCLE_NODE_STARTED)
                return AVERROR(EINVAL);

            // place current position on stack and descend
            av_assert0(nb_filters_stack < sch->nb_filters);
            filters_stack[nb_filters_stack++] = src;
            src = (SchedulerNode){ .idx = node.idx, .idx_stream = 0 };
            continue;
        }

        filters_visited[src.idx] = CYCLE_NODE_DONE;

        // previous search finished,
        if (nb_filters_stack) {
            src = filters_stack[--nb_filters_stack];
            continue;
        }
        return 0;
    }
}

static int check_acyclic(Scheduler *sch)
{
    uint8_t       *filters_visited = NULL;
    SchedulerNode *filters_stack   = NULL;

    int ret = 0;

    if (!sch->nb_filters)
        return 0;

    filters_visited = av_malloc_array(sch->nb_filters, sizeof(*filters_visited));
    if (!filters_visited)
        return AVERROR(ENOMEM);

    filters_stack = av_malloc_array(sch->nb_filters, sizeof(*filters_stack));
    if (!filters_stack) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // trace the transcoding graph upstream from every filtegraph
    for (unsigned i = 0; i < sch->nb_filters; i++) {
        ret = check_acyclic_for_output(sch, (SchedulerNode){ .idx = i },
                                       filters_visited, filters_stack);
        if (ret < 0) {
            av_log(&sch->filters[i], AV_LOG_ERROR, "Transcoding graph has a cycle\n");
            goto fail;
        }
    }

fail:
    av_freep(&filters_visited);
    av_freep(&filters_stack);
    return ret;
}

static int start_prepare(Scheduler *sch)
{
    int ret;

    for (unsigned i = 0; i < sch->nb_demux; i++) {
        SchDemux *d = &sch->demux[i];

        for (unsigned j = 0; j < d->nb_streams; j++) {
            SchDemuxStream *ds = &d->streams[j];

            if (!ds->nb_dst) {
                av_log(d, AV_LOG_ERROR,
                       "Demuxer stream %u not connected to any sink\n", j);
                return AVERROR(EINVAL);
            }

            ds->dst_finished = av_calloc(ds->nb_dst, sizeof(*ds->dst_finished));
            if (!ds->dst_finished)
                return AVERROR(ENOMEM);
        }
    }

    for (unsigned i = 0; i < sch->nb_dec; i++) {
        SchDec *dec = &sch->dec[i];

        if (!dec->src.type) {
            av_log(dec, AV_LOG_ERROR,
                   "Decoder not connected to a source\n");
            return AVERROR(EINVAL);
        }

        for (unsigned j = 0; j < dec->nb_outputs; j++) {
            SchDecOutput *o = &dec->outputs[j];

            if (!o->nb_dst) {
                av_log(dec, AV_LOG_ERROR,
                       "Decoder output %u not connected to any sink\n", j);
                return AVERROR(EINVAL);
            }

            o->dst_finished = av_calloc(o->nb_dst, sizeof(*o->dst_finished));
            if (!o->dst_finished)
                return AVERROR(ENOMEM);
        }
    }

    for (unsigned i = 0; i < sch->nb_enc; i++) {
        SchEnc *enc = &sch->enc[i];

        if (!enc->src.type) {
            av_log(enc, AV_LOG_ERROR,
                   "Encoder not connected to a source\n");
            return AVERROR(EINVAL);
        }
        if (!enc->nb_dst) {
            av_log(enc, AV_LOG_ERROR,
                   "Encoder not connected to any sink\n");
            return AVERROR(EINVAL);
        }

        enc->dst_finished = av_calloc(enc->nb_dst, sizeof(*enc->dst_finished));
        if (!enc->dst_finished)
            return AVERROR(ENOMEM);
    }

    for (unsigned i = 0; i < sch->nb_mux; i++) {
        SchMux *mux = &sch->mux[i];

        for (unsigned j = 0; j < mux->nb_streams; j++) {
            SchMuxStream *ms = &mux->streams[j];

            if (!ms->src.type) {
                av_log(mux, AV_LOG_ERROR,
                       "Muxer stream #%u not connected to a source\n", j);
                return AVERROR(EINVAL);
            }
        }

        ret = queue_alloc(&mux->queue, mux->nb_streams, mux->queue_size,
                          QUEUE_PACKETS);
        if (ret < 0)
            return ret;
    }

    for (unsigned i = 0; i < sch->nb_filters; i++) {
        SchFilterGraph *fg = &sch->filters[i];

        for (unsigned j = 0; j < fg->nb_inputs; j++) {
            SchFilterIn *fi = &fg->inputs[j];

            if (!fi->src.type) {
                av_log(fg, AV_LOG_ERROR,
                       "Filtergraph input %u not connected to a source\n", j);
                return AVERROR(EINVAL);
            }
        }

        for (unsigned j = 0; j < fg->nb_outputs; j++) {
            SchFilterOut *fo = &fg->outputs[j];

            if (!fo->dst.type) {
                av_log(fg, AV_LOG_ERROR,
                       "Filtergraph %u output %u not connected to a sink\n", i, j);
                return AVERROR(EINVAL);
            }
        }
    }

    // Check that the transcoding graph has no cycles.
    ret = check_acyclic(sch);
    if (ret < 0)
        return ret;

    return 0;
}

int sch_start(Scheduler *sch)
{
    int ret;

    ret = start_prepare(sch);
    if (ret < 0)
        return ret;

    av_assert0(sch->state == SCH_STATE_UNINIT);
    sch->state = SCH_STATE_STARTED;

    for (unsigned i = 0; i < sch->nb_mux; i++) {
        SchMux *mux = &sch->mux[i];

        if (mux->nb_streams_ready == mux->nb_streams) {
            ret = mux_init(sch, mux);
            if (ret < 0)
                goto fail;
        }
    }

    for (unsigned i = 0; i < sch->nb_enc; i++) {
        SchEnc *enc = &sch->enc[i];

        ret = task_start(&enc->task);
        if (ret < 0)
            goto fail;
    }

    for (unsigned i = 0; i < sch->nb_filters; i++) {
        SchFilterGraph *fg = &sch->filters[i];

        ret = task_start(&fg->task);
        if (ret < 0)
            goto fail;
    }

    for (unsigned i = 0; i < sch->nb_dec; i++) {
        SchDec *dec = &sch->dec[i];

        ret = task_start(&dec->task);
        if (ret < 0)
            goto fail;
    }

    for (unsigned i = 0; i < sch->nb_demux; i++) {
        SchDemux *d = &sch->demux[i];

        if (!d->nb_streams)
            continue;

        ret = task_start(&d->task);
        if (ret < 0)
            goto fail;
    }

    pthread_mutex_lock(&sch->schedule_lock);
    schedule_update_locked(sch);
    pthread_mutex_unlock(&sch->schedule_lock);

    return 0;
fail:
    sch_stop(sch, NULL);
    return ret;
}

int sch_wait(Scheduler *sch, uint64_t timeout_us, int64_t *transcode_ts)
{
    int ret;

    // convert delay to absolute timestamp
    timeout_us += av_gettime();

    pthread_mutex_lock(&sch->finish_lock);

    if (sch->nb_mux_done < sch->nb_mux) {
        struct timespec tv = { .tv_sec  =  timeout_us / 1000000,
                               .tv_nsec = (timeout_us % 1000000) * 1000 };
        pthread_cond_timedwait(&sch->finish_cond, &sch->finish_lock, &tv);
    }

    // abort transcoding if any task failed
    ret = sch->nb_mux_done == sch->nb_mux || sch->task_failed;

    pthread_mutex_unlock(&sch->finish_lock);

    *transcode_ts = atomic_load(&sch->last_dts);

    return ret;
}

static int sched_local_enc_open(Scheduler *sch, SchEnc *enc, const AVFrame *frame)
{
    int ret;

    ret = enc->open_cb(enc->task.func_arg, frame);
    if (ret < 0)
        return ret;

    // ret>0 signals audio frame size, which means sync queue must
    // have been enabled during encoder creation
    if (ret > 0) {
        SchSyncQueue *sq;

        av_assert0(enc->sq_idx[0] >= 0);
        sq = &sch->sq_enc[enc->sq_idx[0]];

        pthread_mutex_lock(&sq->lock);

        sq_frame_samples(sq->sq, enc->sq_idx[1], ret);

        pthread_mutex_unlock(&sq->lock);
    }

    return 0;
}

static int send_to_enc_thread(Scheduler *sch, SchEnc *enc, AVFrame *frame)
{
    int ret;

    if (!frame) {
        tq_send_finish(enc->queue, 0);
        return 0;
    }

    if (enc->in_finished)
        return AVERROR_EOF;

    ret = tq_send(enc->queue, 0, frame);
    if (ret < 0)
        enc->in_finished = 1;

    return ret;
}

static int send_to_enc_sq(Scheduler *sch, SchEnc *enc, AVFrame *frame)
{
    SchSyncQueue *sq = &sch->sq_enc[enc->sq_idx[0]];
    int ret = 0;

    // inform the scheduling code that no more input will arrive along this path;
    // this is necessary because the sync queue may not send an EOF downstream
    // until other streams finish
    // TODO: consider a cleaner way of passing this information through
    //       the pipeline
    if (!frame) {
        for (unsigned i = 0; i < enc->nb_dst; i++) {
            SchMux      *mux;
            SchMuxStream *ms;

            if (enc->dst[i].type != SCH_NODE_TYPE_MUX)
                continue;

            mux = &sch->mux[enc->dst[i].idx];
            ms = &mux->streams[enc->dst[i].idx_stream];

            pthread_mutex_lock(&sch->schedule_lock);

            ms->source_finished = 1;
            schedule_update_locked(sch);

            pthread_mutex_unlock(&sch->schedule_lock);
        }
    }

    pthread_mutex_lock(&sq->lock);

    ret = sq_send(sq->sq, enc->sq_idx[1], SQFRAME(frame));
    if (ret < 0)
        goto finish;

    while (1) {
        SchEnc *enc;

        // TODO: the SQ API should be extended to allow returning EOF
        // for individual streams
        ret = sq_receive(sq->sq, -1, SQFRAME(sq->frame));
        if (ret < 0) {
            ret = (ret == AVERROR(EAGAIN)) ? 0 : ret;
            break;
        }

        enc = &sch->enc[sq->enc_idx[ret]];
        ret = send_to_enc_thread(sch, enc, sq->frame);
        if (ret < 0) {
            av_frame_unref(sq->frame);
            if (ret != AVERROR_EOF)
                break;

            sq_send(sq->sq, enc->sq_idx[1], SQFRAME(NULL));
            continue;
        }
    }

    if (ret < 0) {
        // close all encoders fed from this sync queue
        for (unsigned i = 0; i < sq->nb_enc_idx; i++) {
            int err = send_to_enc_thread(sch, &sch->enc[sq->enc_idx[i]], NULL);

            // if the sync queue error is EOF and closing the encoder
            // produces a more serious error, make sure to pick the latter
            ret = err_merge((ret == AVERROR_EOF && err < 0) ? 0 : ret, err);
        }
    }

finish:
    pthread_mutex_unlock(&sq->lock);

    return ret;
}

static int send_to_enc(Scheduler *sch, SchEnc *enc, AVFrame *frame)
{
    if (enc->open_cb && frame && !enc->opened) {
        int ret = sched_local_enc_open(sch, enc, frame);
        if (ret < 0)
            return ret;
        enc->opened = 1;

        // discard empty frames that only carry encoder init parameters
        if (!frame->buf[0]) {
            av_frame_unref(frame);
            return 0;
        }
    }

    return (enc->sq_idx[0] >= 0)                ?
           send_to_enc_sq    (sch, enc, frame)  :
           send_to_enc_thread(sch, enc, frame);
}

static int mux_queue_packet(SchMux *mux, SchMuxStream *ms, AVPacket *pkt)
{
    PreMuxQueue *q = &ms->pre_mux_queue;
    AVPacket *tmp_pkt = NULL;
    int ret;

    if (!av_fifo_can_write(q->fifo)) {
        size_t     packets = av_fifo_can_read(q->fifo);
        size_t    pkt_size = pkt ? pkt->size : 0;
        int thresh_reached = (q->data_size + pkt_size) > q->data_threshold;
        size_t max_packets = thresh_reached ? q->max_packets : SIZE_MAX;
        size_t new_size = FFMIN(2 * packets, max_packets);

        if (new_size <= packets) {
            av_log(mux, AV_LOG_ERROR,
                   "Too many packets buffered for output stream.\n");
            return AVERROR_BUFFER_TOO_SMALL;
        }
        ret = av_fifo_grow2(q->fifo, new_size - packets);
        if (ret < 0)
            return ret;
    }

    if (pkt) {
        tmp_pkt = av_packet_alloc();
        if (!tmp_pkt)
            return AVERROR(ENOMEM);

        av_packet_move_ref(tmp_pkt, pkt);
        q->data_size += tmp_pkt->size;
    }
    av_fifo_write(q->fifo, &tmp_pkt, 1);

    return 0;
}

static int send_to_mux(Scheduler *sch, SchMux *mux, unsigned stream_idx,
                       AVPacket *pkt)
{
    SchMuxStream *ms = &mux->streams[stream_idx];
    int64_t dts = (pkt && pkt->dts != AV_NOPTS_VALUE)                                    ?
                  av_rescale_q(pkt->dts + pkt->duration, pkt->time_base, AV_TIME_BASE_Q) :
                  AV_NOPTS_VALUE;

    // queue the packet if the muxer cannot be started yet
    if (!atomic_load(&mux->mux_started)) {
        int queued = 0;

        // the muxer could have started between the above atomic check and
        // locking the mutex, then this block falls through to normal send path
        pthread_mutex_lock(&sch->mux_ready_lock);

        if (!atomic_load(&mux->mux_started)) {
            int ret = mux_queue_packet(mux, ms, pkt);
            queued = ret < 0 ? ret : 1;
        }

        pthread_mutex_unlock(&sch->mux_ready_lock);

        if (queued < 0)
            return queued;
        else if (queued)
            goto update_schedule;
    }

    if (pkt) {
        int ret;

        if (ms->init_eof)
            return AVERROR_EOF;

        ret = tq_send(mux->queue, stream_idx, pkt);
        if (ret < 0)
            return ret;
    } else
        tq_send_finish(mux->queue, stream_idx);

update_schedule:
    // TODO: use atomics to check whether this changes trailing dts
    // to avoid locking unnecessarily
    if (dts != AV_NOPTS_VALUE || !pkt) {
        pthread_mutex_lock(&sch->schedule_lock);

        if (pkt) ms->last_dts = dts;
        else     ms->source_finished = 1;

        schedule_update_locked(sch);

        pthread_mutex_unlock(&sch->schedule_lock);
    }

    return 0;
}

static int
demux_stream_send_to_dst(Scheduler *sch, const SchedulerNode dst,
                         uint8_t *dst_finished, AVPacket *pkt, unsigned flags)
{
    int ret;

    if (*dst_finished)
        return AVERROR_EOF;

    if (pkt && dst.type == SCH_NODE_TYPE_MUX &&
        (flags & DEMUX_SEND_STREAMCOPY_EOF)) {
        av_packet_unref(pkt);
        pkt = NULL;
    }

    if (!pkt)
        goto finish;

    ret = (dst.type == SCH_NODE_TYPE_MUX) ?
          send_to_mux(sch, &sch->mux[dst.idx], dst.idx_stream, pkt) :
          tq_send(sch->dec[dst.idx].queue, 0, pkt);
    if (ret == AVERROR_EOF)
        goto finish;

    return ret;

finish:
    if (dst.type == SCH_NODE_TYPE_MUX)
        send_to_mux(sch, &sch->mux[dst.idx], dst.idx_stream, NULL);
    else
        tq_send_finish(sch->dec[dst.idx].queue, 0);

    *dst_finished = 1;
    return AVERROR_EOF;
}

static int demux_send_for_stream(Scheduler *sch, SchDemux *d, SchDemuxStream *ds,
                                 AVPacket *pkt, unsigned flags)
{
    unsigned nb_done = 0;

    for (unsigned i = 0; i < ds->nb_dst; i++) {
        AVPacket *to_send = pkt;
        uint8_t *finished = &ds->dst_finished[i];

        int ret;

        // sending a packet consumes it, so make a temporary reference if needed
        if (pkt && i < ds->nb_dst - 1) {
            to_send = d->send_pkt;

            ret = av_packet_ref(to_send, pkt);
            if (ret < 0)
                return ret;
        }

        ret = demux_stream_send_to_dst(sch, ds->dst[i], finished, to_send, flags);
        if (to_send)
            av_packet_unref(to_send);
        if (ret == AVERROR_EOF)
            nb_done++;
        else if (ret < 0)
            return ret;
    }

    return (nb_done == ds->nb_dst) ? AVERROR_EOF : 0;
}

static int demux_flush(Scheduler *sch, SchDemux *d, AVPacket *pkt)
{
    Timestamp max_end_ts = (Timestamp){ .ts = AV_NOPTS_VALUE };

    av_assert0(!pkt->buf && !pkt->data && !pkt->side_data_elems);

    for (unsigned i = 0; i < d->nb_streams; i++) {
        SchDemuxStream *ds = &d->streams[i];

        for (unsigned j = 0; j < ds->nb_dst; j++) {
            const SchedulerNode *dst = &ds->dst[j];
            SchDec *dec;
            int ret;

            if (ds->dst_finished[j] || dst->type != SCH_NODE_TYPE_DEC)
                continue;

            dec = &sch->dec[dst->idx];

            ret = tq_send(dec->queue, 0, pkt);
            if (ret < 0)
                return ret;

            if (dec->queue_end_ts) {
                Timestamp ts;
                ret = av_thread_message_queue_recv(dec->queue_end_ts, &ts, 0);
                if (ret < 0)
                    return ret;

                if (max_end_ts.ts == AV_NOPTS_VALUE ||
                    (ts.ts != AV_NOPTS_VALUE &&
                     av_compare_ts(max_end_ts.ts, max_end_ts.tb, ts.ts, ts.tb) < 0))
                    max_end_ts = ts;

            }
        }
    }

    pkt->pts       = max_end_ts.ts;
    pkt->time_base = max_end_ts.tb;

    return 0;
}

int sch_demux_send(Scheduler *sch, unsigned demux_idx, AVPacket *pkt,
                   unsigned flags)
{
    SchDemux *d;
    int terminate;

    av_assert0(demux_idx < sch->nb_demux);
    d = &sch->demux[demux_idx];

    terminate = waiter_wait(sch, &d->waiter);
    if (terminate)
        return AVERROR_EXIT;

    // flush the downstreams after seek
    if (pkt->stream_index == -1)
        return demux_flush(sch, d, pkt);

    av_assert0(pkt->stream_index < d->nb_streams);

    return demux_send_for_stream(sch, d, &d->streams[pkt->stream_index], pkt, flags);
}

static int demux_done(Scheduler *sch, unsigned demux_idx)
{
    SchDemux *d = &sch->demux[demux_idx];
    int ret = 0;

    for (unsigned i = 0; i < d->nb_streams; i++) {
        int err = demux_send_for_stream(sch, d, &d->streams[i], NULL, 0);
        if (err != AVERROR_EOF)
            ret = err_merge(ret, err);
    }

    pthread_mutex_lock(&sch->schedule_lock);

    d->task_exited = 1;

    schedule_update_locked(sch);

    pthread_mutex_unlock(&sch->schedule_lock);

    return ret;
}

int sch_mux_receive(Scheduler *sch, unsigned mux_idx, AVPacket *pkt)
{
    SchMux *mux;
    int ret, stream_idx;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    ret = tq_receive(mux->queue, &stream_idx, pkt);
    pkt->stream_index = stream_idx;
    return ret;
}

void sch_mux_receive_finish(Scheduler *sch, unsigned mux_idx, unsigned stream_idx)
{
    SchMux *mux;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    av_assert0(stream_idx < mux->nb_streams);
    tq_receive_finish(mux->queue, stream_idx);

    pthread_mutex_lock(&sch->schedule_lock);
    mux->streams[stream_idx].source_finished = 1;

    schedule_update_locked(sch);

    pthread_mutex_unlock(&sch->schedule_lock);
}

int sch_mux_sub_heartbeat(Scheduler *sch, unsigned mux_idx, unsigned stream_idx,
                          const AVPacket *pkt)
{
    SchMux       *mux;
    SchMuxStream *ms;

    av_assert0(mux_idx < sch->nb_mux);
    mux = &sch->mux[mux_idx];

    av_assert0(stream_idx < mux->nb_streams);
    ms = &mux->streams[stream_idx];

    for (unsigned i = 0; i < ms->nb_sub_heartbeat_dst; i++) {
        SchDec *dst = &sch->dec[ms->sub_heartbeat_dst[i]];
        int ret;

        ret = av_packet_copy_props(mux->sub_heartbeat_pkt, pkt);
        if (ret < 0)
            return ret;

        tq_send(dst->queue, 0, mux->sub_heartbeat_pkt);
    }

    return 0;
}

static int mux_done(Scheduler *sch, unsigned mux_idx)
{
    SchMux *mux = &sch->mux[mux_idx];

    pthread_mutex_lock(&sch->schedule_lock);

    for (unsigned i = 0; i < mux->nb_streams; i++) {
        tq_receive_finish(mux->queue, i);
        mux->streams[i].source_finished = 1;
    }

    schedule_update_locked(sch);

    pthread_mutex_unlock(&sch->schedule_lock);

    pthread_mutex_lock(&sch->finish_lock);

    av_assert0(sch->nb_mux_done < sch->nb_mux);
    sch->nb_mux_done++;

    pthread_cond_signal(&sch->finish_cond);

    pthread_mutex_unlock(&sch->finish_lock);

    return 0;
}

int sch_dec_receive(Scheduler *sch, unsigned dec_idx, AVPacket *pkt)
{
    SchDec *dec;
    int ret, dummy;

    av_assert0(dec_idx < sch->nb_dec);
    dec = &sch->dec[dec_idx];

    // the decoder should have given us post-flush end timestamp in pkt
    if (dec->expect_end_ts) {
        Timestamp ts = (Timestamp){ .ts = pkt->pts, .tb = pkt->time_base };
        ret = av_thread_message_queue_send(dec->queue_end_ts, &ts, 0);
        if (ret < 0)
            return ret;

        dec->expect_end_ts = 0;
    }

    ret = tq_receive(dec->queue, &dummy, pkt);
    av_assert0(dummy <= 0);

    // got a flush packet, on the next call to this function the decoder
    // will give us post-flush end timestamp
    if (ret >= 0 && !pkt->data && !pkt->side_data_elems && dec->queue_end_ts)
        dec->expect_end_ts = 1;

    return ret;
}

static int send_to_filter(Scheduler *sch, SchFilterGraph *fg,
                          unsigned in_idx, AVFrame *frame)
{
    if (frame)
        return tq_send(fg->queue, in_idx, frame);

    pthread_mutex_lock(&sch->schedule_lock);

    if (!fg->inputs[in_idx].send_finished) {
        fg->inputs[in_idx].send_finished = 1;
        tq_send_finish(fg->queue, in_idx);

        // close the control stream when all actual inputs are done
        if (++fg->nb_inputs_finished_send == fg->nb_inputs)
            tq_send_finish(fg->queue, fg->nb_inputs);

        schedule_update_locked(sch);
    }

    pthread_mutex_unlock(&sch->schedule_lock);
    return 0;
}

static int dec_send_to_dst(Scheduler *sch, const SchedulerNode dst,
                           uint8_t *dst_finished, AVFrame *frame)
{
    int ret;

    if (*dst_finished)
        return AVERROR_EOF;

    if (!frame)
        goto finish;

    ret = (dst.type == SCH_NODE_TYPE_FILTER_IN) ?
          send_to_filter(sch, &sch->filters[dst.idx], dst.idx_stream, frame) :
          send_to_enc(sch, &sch->enc[dst.idx], frame);
    if (ret == AVERROR_EOF)
        goto finish;

    return ret;

finish:
    if (dst.type == SCH_NODE_TYPE_FILTER_IN)
        send_to_filter(sch, &sch->filters[dst.idx], dst.idx_stream, NULL);
    else
        send_to_enc(sch, &sch->enc[dst.idx], NULL);

    *dst_finished = 1;

    return AVERROR_EOF;
}

int sch_dec_send(Scheduler *sch, unsigned dec_idx,
                 unsigned out_idx, AVFrame *frame)
{
    SchDec *dec;
    SchDecOutput *o;
    int ret;
    unsigned nb_done = 0;

    av_assert0(dec_idx < sch->nb_dec);
    dec = &sch->dec[dec_idx];

    av_assert0(out_idx < dec->nb_outputs);
    o = &dec->outputs[out_idx];

    for (unsigned i = 0; i < o->nb_dst; i++) {
        uint8_t *finished = &o->dst_finished[i];
        AVFrame *to_send  = frame;

        // sending a frame consumes it, so make a temporary reference if needed
        if (i < o->nb_dst - 1) {
            to_send = dec->send_frame;

            // frame may sometimes contain props only,
            // e.g. to signal EOF timestamp
            ret = frame->buf[0] ? av_frame_ref(to_send, frame) :
                                  av_frame_copy_props(to_send, frame);
            if (ret < 0)
                return ret;
        }

        ret = dec_send_to_dst(sch, o->dst[i], finished, to_send);
        if (ret < 0) {
            av_frame_unref(to_send);
            if (ret == AVERROR_EOF) {
                nb_done++;
                continue;
            }
            return ret;
        }
    }

    return (nb_done == o->nb_dst) ? AVERROR_EOF : 0;
}

static int dec_done(Scheduler *sch, unsigned dec_idx)
{
    SchDec *dec = &sch->dec[dec_idx];
    int ret = 0;

    tq_receive_finish(dec->queue, 0);

    // make sure our source does not get stuck waiting for end timestamps
    // that will never arrive
    if (dec->queue_end_ts)
        av_thread_message_queue_set_err_recv(dec->queue_end_ts, AVERROR_EOF);

    for (unsigned i = 0; i < dec->nb_outputs; i++) {
        SchDecOutput *o = &dec->outputs[i];

        for (unsigned j = 0; j < o->nb_dst; j++) {
            int err = dec_send_to_dst(sch, o->dst[j], &o->dst_finished[j], NULL);
            if (err < 0 && err != AVERROR_EOF)
                ret = err_merge(ret, err);
        }
    }

    return ret;
}

int sch_enc_receive(Scheduler *sch, unsigned enc_idx, AVFrame *frame)
{
    SchEnc *enc;
    int ret, dummy;

    av_assert0(enc_idx < sch->nb_enc);
    enc = &sch->enc[enc_idx];

    ret = tq_receive(enc->queue, &dummy, frame);
    av_assert0(dummy <= 0);

    return ret;
}

static int enc_send_to_dst(Scheduler *sch, const SchedulerNode dst,
                           uint8_t *dst_finished, AVPacket *pkt)
{
    int ret;

    if (*dst_finished)
        return AVERROR_EOF;

    if (!pkt)
        goto finish;

    ret = (dst.type == SCH_NODE_TYPE_MUX) ?
          send_to_mux(sch, &sch->mux[dst.idx], dst.idx_stream, pkt) :
          tq_send(sch->dec[dst.idx].queue, 0, pkt);
    if (ret == AVERROR_EOF)
        goto finish;

    return ret;

finish:
    if (dst.type == SCH_NODE_TYPE_MUX)
        send_to_mux(sch, &sch->mux[dst.idx], dst.idx_stream, NULL);
    else
        tq_send_finish(sch->dec[dst.idx].queue, 0);

    *dst_finished = 1;

    return AVERROR_EOF;
}

int sch_enc_send(Scheduler *sch, unsigned enc_idx, AVPacket *pkt)
{
    SchEnc *enc;
    int ret;

    av_assert0(enc_idx < sch->nb_enc);
    enc = &sch->enc[enc_idx];

    for (unsigned i = 0; i < enc->nb_dst; i++) {
        uint8_t *finished = &enc->dst_finished[i];
        AVPacket *to_send = pkt;

        // sending a packet consumes it, so make a temporary reference if needed
        if (i < enc->nb_dst - 1) {
            to_send = enc->send_pkt;

            ret = av_packet_ref(to_send, pkt);
            if (ret < 0)
                return ret;
        }

        ret = enc_send_to_dst(sch, enc->dst[i], finished, to_send);
        if (ret < 0) {
            av_packet_unref(to_send);
            if (ret == AVERROR_EOF)
                continue;
            return ret;
        }
    }

    return 0;
}

static int enc_done(Scheduler *sch, unsigned enc_idx)
{
    SchEnc *enc = &sch->enc[enc_idx];
    int ret = 0;

    tq_receive_finish(enc->queue, 0);

    for (unsigned i = 0; i < enc->nb_dst; i++) {
        int err = enc_send_to_dst(sch, enc->dst[i], &enc->dst_finished[i], NULL);
        if (err < 0 && err != AVERROR_EOF)
            ret = err_merge(ret, err);
    }

    return ret;
}

int sch_filter_receive(Scheduler *sch, unsigned fg_idx,
                       unsigned *in_idx, AVFrame *frame)
{
    SchFilterGraph *fg;

    av_assert0(fg_idx < sch->nb_filters);
    fg = &sch->filters[fg_idx];

    av_assert0(*in_idx <= fg->nb_inputs);

    // update scheduling to account for desired input stream, if it changed
    //
    // this check needs no locking because only the filtering thread
    // updates this value
    if (*in_idx != fg->best_input) {
        pthread_mutex_lock(&sch->schedule_lock);

        fg->best_input = *in_idx;
        schedule_update_locked(sch);

        pthread_mutex_unlock(&sch->schedule_lock);
    }

    if (*in_idx == fg->nb_inputs) {
        int terminate = waiter_wait(sch, &fg->waiter);
        return terminate ? AVERROR_EOF : AVERROR(EAGAIN);
    }

    while (1) {
        int ret, idx;

        ret = tq_receive(fg->queue, &idx, frame);
        if (idx < 0)
            return AVERROR_EOF;
        else if (ret >= 0) {
            *in_idx = idx;
            return 0;
        }

        // disregard EOFs for specific streams - they should always be
        // preceded by an EOF frame
    }
}

void sch_filter_receive_finish(Scheduler *sch, unsigned fg_idx, unsigned in_idx)
{
    SchFilterGraph *fg;
    SchFilterIn    *fi;

    av_assert0(fg_idx < sch->nb_filters);
    fg = &sch->filters[fg_idx];

    av_assert0(in_idx < fg->nb_inputs);
    fi = &fg->inputs[in_idx];

    pthread_mutex_lock(&sch->schedule_lock);

    if (!fi->receive_finished) {
        fi->receive_finished = 1;
        tq_receive_finish(fg->queue, in_idx);

        // close the control stream when all actual inputs are done
        if (++fg->nb_inputs_finished_receive == fg->nb_inputs)
            tq_receive_finish(fg->queue, fg->nb_inputs);

        schedule_update_locked(sch);
    }

    pthread_mutex_unlock(&sch->schedule_lock);
}

int sch_filter_send(Scheduler *sch, unsigned fg_idx, unsigned out_idx, AVFrame *frame)
{
    SchFilterGraph *fg;
    SchedulerNode  dst;
    int ret;

    av_assert0(fg_idx < sch->nb_filters);
    fg = &sch->filters[fg_idx];

    av_assert0(out_idx < fg->nb_outputs);
    dst = fg->outputs[out_idx].dst;

    if (dst.type == SCH_NODE_TYPE_ENC) {
        ret = send_to_enc(sch, &sch->enc[dst.idx], frame);
        if (ret == AVERROR_EOF)
            send_to_enc(sch, &sch->enc[dst.idx], NULL);
    } else {
        ret = send_to_filter(sch, &sch->filters[dst.idx], dst.idx_stream, frame);
        if (ret == AVERROR_EOF)
            send_to_filter(sch, &sch->filters[dst.idx], dst.idx_stream, NULL);
    }
    return ret;
}

static int filter_done(Scheduler *sch, unsigned fg_idx)
{
    SchFilterGraph *fg = &sch->filters[fg_idx];
    int ret = 0;

    for (unsigned i = 0; i <= fg->nb_inputs; i++)
        tq_receive_finish(fg->queue, i);

    for (unsigned i = 0; i < fg->nb_outputs; i++) {
        SchedulerNode dst = fg->outputs[i].dst;
        int err = (dst.type == SCH_NODE_TYPE_ENC)                                   ?
                  send_to_enc   (sch, &sch->enc[dst.idx],                     NULL) :
                  send_to_filter(sch, &sch->filters[dst.idx], dst.idx_stream, NULL);

        if (err < 0 && err != AVERROR_EOF)
            ret = err_merge(ret, err);
    }

    pthread_mutex_lock(&sch->schedule_lock);

    fg->task_exited = 1;

    schedule_update_locked(sch);

    pthread_mutex_unlock(&sch->schedule_lock);

    return ret;
}

int sch_filter_command(Scheduler *sch, unsigned fg_idx, AVFrame *frame)
{
    SchFilterGraph *fg;

    av_assert0(fg_idx < sch->nb_filters);
    fg = &sch->filters[fg_idx];

    return send_to_filter(sch, fg, fg->nb_inputs, frame);
}

void sch_filter_choke_inputs(Scheduler *sch, unsigned fg_idx)
{
    SchFilterGraph *fg;
    av_assert0(fg_idx < sch->nb_filters);
    fg = &sch->filters[fg_idx];

    pthread_mutex_lock(&sch->schedule_lock);
    fg->best_input = fg->nb_inputs;
    schedule_update_locked(sch);
    pthread_mutex_unlock(&sch->schedule_lock);
}

static int task_cleanup(Scheduler *sch, SchedulerNode node)
{
    switch (node.type) {
    case SCH_NODE_TYPE_DEMUX:       return demux_done (sch, node.idx);
    case SCH_NODE_TYPE_MUX:         return mux_done   (sch, node.idx);
    case SCH_NODE_TYPE_DEC:         return dec_done   (sch, node.idx);
    case SCH_NODE_TYPE_ENC:         return enc_done   (sch, node.idx);
    case SCH_NODE_TYPE_FILTER_IN:   return filter_done(sch, node.idx);
    default: av_unreachable("Invalid node type?");
    }
}

static void *task_wrapper(void *arg)
{
    SchTask  *task = arg;
    Scheduler *sch = task->parent;
    int ret;
    int err = 0;

    ret = task->func(task->func_arg);
    if (ret < 0)
        av_log(task->func_arg, AV_LOG_ERROR,
               "Task finished with error code: %d (%s)\n", ret, av_err2str(ret));

    err = task_cleanup(sch, task->node);
    ret = err_merge(ret, err);

    // EOF is considered normal termination
    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret < 0) {
        pthread_mutex_lock(&sch->finish_lock);
        sch->task_failed = 1;
        pthread_cond_signal(&sch->finish_cond);
        pthread_mutex_unlock(&sch->finish_lock);
    }

    av_log(task->func_arg, ret < 0 ? AV_LOG_ERROR : AV_LOG_VERBOSE,
           "Terminating thread with return code %d (%s)\n", ret,
           ret < 0 ? av_err2str(ret) : "success");

    return (void*)(intptr_t)ret;
}

static int task_stop(Scheduler *sch, SchTask *task)
{
    int ret;
    void *thread_ret;

    if (!task->parent)
        return 0;

    if (!task->thread_running)
        return task_cleanup(sch, task->node);

    ret = pthread_join(task->thread, &thread_ret);
    av_assert0(ret == 0);

    task->thread_running = 0;

    return (intptr_t)thread_ret;
}

int sch_stop(Scheduler *sch, int64_t *finish_ts)
{
    int ret = 0, err;

    if (sch->state != SCH_STATE_STARTED)
        return 0;

    atomic_store(&sch->terminate, 1);

    for (unsigned type = 0; type < 2; type++)
        for (unsigned i = 0; i < (type ? sch->nb_demux : sch->nb_filters); i++) {
            SchWaiter *w = type ? &sch->demux[i].waiter : &sch->filters[i].waiter;
            waiter_set(w, 1);
            if (type)
                choke_demux(sch, i, 0); // unfreeze to allow draining
        }

    for (unsigned i = 0; i < sch->nb_demux; i++) {
        SchDemux *d = &sch->demux[i];

        err = task_stop(sch, &d->task);
        ret = err_merge(ret, err);
    }

    for (unsigned i = 0; i < sch->nb_dec; i++) {
        SchDec *dec = &sch->dec[i];

        err = task_stop(sch, &dec->task);
        ret = err_merge(ret, err);
    }

    for (unsigned i = 0; i < sch->nb_filters; i++) {
        SchFilterGraph *fg = &sch->filters[i];

        err = task_stop(sch, &fg->task);
        ret = err_merge(ret, err);
    }

    for (unsigned i = 0; i < sch->nb_enc; i++) {
        SchEnc *enc = &sch->enc[i];

        err = task_stop(sch, &enc->task);
        ret = err_merge(ret, err);
    }

    for (unsigned i = 0; i < sch->nb_mux; i++) {
        SchMux *mux = &sch->mux[i];

        err = task_stop(sch, &mux->task);
        ret = err_merge(ret, err);
    }

    if (finish_ts)
        *finish_ts = trailing_dts(sch, 1);

    sch->state = SCH_STATE_STOPPED;

    return ret;
}


/* ========== fftools/ffmpeg_dec.c ========== */

/*
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

#include <stdbit.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/stereo3d.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/codec.h"

#include "ffmpeg.h"

typedef struct DecoderPriv {
    Decoder             dec;

    AVCodecContext     *dec_ctx;

    AVFrame            *frame;
    AVFrame            *frame_tmp_ref;
    AVPacket           *pkt;

    // override output video sample aspect ratio with this value
    AVRational          sar_override;

    AVRational          framerate_in;

    // a combination of DECODER_FLAG_*, provided to dec_open()
    int                 flags;
    int                 apply_cropping;

    enum AVPixelFormat  hwaccel_pix_fmt;
    enum HWAccelID      hwaccel_id;
    enum AVHWDeviceType hwaccel_device_type;
    enum AVPixelFormat  hwaccel_output_format;

    // pts/estimated duration of the last decoded frame
    // * in decoder timebase for video,
    // * in last_frame_tb (may change during decoding) for audio
    int64_t             last_frame_pts;
    int64_t             last_frame_duration_est;
    AVRational          last_frame_tb;
    int64_t             last_filter_in_rescale_delta;
    int                 last_frame_sample_rate;

    /* previous decoded subtitles */
    AVFrame            *sub_prev[2];
    AVFrame            *sub_heartbeat;

    Scheduler          *sch;
    unsigned            sch_idx;

    // this decoder's index in decoders or -1
    int                 index;
    void               *log_parent;
    char                log_name[32];
    char               *parent_name;

    // user specified decoder multiview options manually
    int                 multiview_user_config;

    struct {
        ViewSpecifier   vs;
        unsigned        out_idx;
    }                  *views_requested;
    int              nb_views_requested;

    /* A map of view ID to decoder outputs.
     * MUST NOT be accessed outside of get_format()/get_buffer() */
    struct {
        unsigned        id;
        uintptr_t       out_mask;
    }                  *view_map;
    int              nb_view_map;

    struct {
        AVDictionary       *opts;
        const AVCodec      *codec;
    } standalone_init;
} DecoderPriv;

static DecoderPriv *dp_from_dec(Decoder *d)
{
    return (DecoderPriv*)d;
}

// data that is local to the decoder thread and not visible outside of it
typedef struct DecThreadContext {
    AVFrame         *frame;
    AVPacket        *pkt;
} DecThreadContext;

void dec_free(Decoder **pdec)
{
    Decoder *dec = *pdec;
    DecoderPriv *dp;

    if (!dec)
        return;
    dp = dp_from_dec(dec);

    avcodec_free_context(&dp->dec_ctx);

    av_frame_free(&dp->frame);
    av_frame_free(&dp->frame_tmp_ref);
    av_packet_free(&dp->pkt);

    av_dict_free(&dp->standalone_init.opts);

    for (int i = 0; i < FF_ARRAY_ELEMS(dp->sub_prev); i++)
        av_frame_free(&dp->sub_prev[i]);
    av_frame_free(&dp->sub_heartbeat);

    av_freep(&dp->parent_name);

    av_freep(&dp->views_requested);
    av_freep(&dp->view_map);

    av_freep(pdec);
}

static const char *dec_item_name(void *obj)
{
    const DecoderPriv *dp = obj;

    return dp->log_name;
}

static const AVClass dec_class = {
    .class_name                = "Decoder",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(DecoderPriv, log_parent),
    .item_name                 = dec_item_name,
};

static int decoder_thread(void *arg);

static int dec_alloc(DecoderPriv **pdec, Scheduler *sch, int send_end_ts)
{
    DecoderPriv *dp;
    int ret = 0;

    *pdec = NULL;

    dp = av_mallocz(sizeof(*dp));
    if (!dp)
        return AVERROR(ENOMEM);

    dp->frame = av_frame_alloc();
    if (!dp->frame)
        goto fail;

    dp->pkt = av_packet_alloc();
    if (!dp->pkt)
        goto fail;

    dp->index                        = -1;
    dp->dec.class                    = &dec_class;
    dp->last_filter_in_rescale_delta = AV_NOPTS_VALUE;
    dp->last_frame_pts               = AV_NOPTS_VALUE;
    dp->last_frame_tb                = (AVRational){ 1, 1 };
    dp->hwaccel_pix_fmt              = AV_PIX_FMT_NONE;

    ret = sch_add_dec(sch, decoder_thread, dp, send_end_ts);
    if (ret < 0)
        goto fail;
    dp->sch     = sch;
    dp->sch_idx = ret;

    *pdec = dp;

    return 0;
fail:
    dec_free((Decoder**)&dp);
    return ret >= 0 ? AVERROR(ENOMEM) : ret;
}

static AVRational audio_samplerate_update(DecoderPriv *dp,
                                          const AVFrame *frame)
{
    const int prev = dp->last_frame_tb.den;
    const int sr   = frame->sample_rate;

    AVRational tb_new;
    int64_t gcd;

    if (frame->sample_rate == dp->last_frame_sample_rate)
        goto finish;

    gcd  = av_gcd(prev, sr);

    if (prev / gcd >= INT_MAX / sr) {
        av_log(dp, AV_LOG_WARNING,
               "Audio timestamps cannot be represented exactly after "
               "sample rate change: %d -> %d\n", prev, sr);

        // LCM of 192000, 44100, allows to represent all common samplerates
        tb_new = (AVRational){ 1, 28224000 };
    } else
        tb_new = (AVRational){ 1, prev / gcd * sr };

    // keep the frame timebase if it is strictly better than
    // the samplerate-defined one
    if (frame->time_base.num == 1 && frame->time_base.den > tb_new.den &&
        !(frame->time_base.den % tb_new.den))
        tb_new = frame->time_base;

    if (dp->last_frame_pts != AV_NOPTS_VALUE)
        dp->last_frame_pts = av_rescale_q(dp->last_frame_pts,
                                          dp->last_frame_tb, tb_new);
    dp->last_frame_duration_est = av_rescale_q(dp->last_frame_duration_est,
                                               dp->last_frame_tb, tb_new);

    dp->last_frame_tb          = tb_new;
    dp->last_frame_sample_rate = frame->sample_rate;

finish:
    return dp->last_frame_tb;
}

static void audio_ts_process(DecoderPriv *dp, AVFrame *frame)
{
    AVRational tb_filter = (AVRational){1, frame->sample_rate};
    AVRational tb;
    int64_t pts_pred;

    // on samplerate change, choose a new internal timebase for timestamp
    // generation that can represent timestamps from all the samplerates
    // seen so far
    tb = audio_samplerate_update(dp, frame);
    pts_pred = dp->last_frame_pts == AV_NOPTS_VALUE ? 0 :
               dp->last_frame_pts + dp->last_frame_duration_est;

    if (frame->pts == AV_NOPTS_VALUE) {
        frame->pts = pts_pred;
        frame->time_base = tb;
    } else if (dp->last_frame_pts != AV_NOPTS_VALUE &&
               frame->pts > av_rescale_q_rnd(pts_pred, tb, frame->time_base,
                                             AV_ROUND_UP)) {
        // there was a gap in timestamps, reset conversion state
        dp->last_filter_in_rescale_delta = AV_NOPTS_VALUE;
    }

    frame->pts = av_rescale_delta(frame->time_base, frame->pts,
                                  tb, frame->nb_samples,
                                  &dp->last_filter_in_rescale_delta, tb);

    dp->last_frame_pts          = frame->pts;
    dp->last_frame_duration_est = av_rescale_q(frame->nb_samples,
                                               tb_filter, tb);

    // finally convert to filtering timebase
    frame->pts       = av_rescale_q(frame->pts, tb, tb_filter);
    frame->duration  = frame->nb_samples;
    frame->time_base = tb_filter;
}

static int64_t video_duration_estimate(const DecoderPriv *dp, const AVFrame *frame)
{
    const int  ts_unreliable = dp->flags & DECODER_FLAG_TS_UNRELIABLE;
    const int      fr_forced = dp->flags & DECODER_FLAG_FRAMERATE_FORCED;
    int64_t codec_duration = 0;
    // difference between this and last frame's timestamps
    const int64_t ts_diff =
        (frame->pts != AV_NOPTS_VALUE && dp->last_frame_pts != AV_NOPTS_VALUE) ?
        frame->pts - dp->last_frame_pts : -1;

    // XXX lavf currently makes up frame durations when they are not provided by
    // the container. As there is no way to reliably distinguish real container
    // durations from the fake made-up ones, we use heuristics based on whether
    // the container has timestamps. Eventually lavf should stop making up
    // durations, then this should be simplified.

    // frame duration is unreliable (typically guessed by lavf) when it is equal
    // to 1 and the actual duration of the last frame is more than 2x larger
    const int duration_unreliable = frame->duration == 1 && ts_diff > 2 * frame->duration;

    // prefer frame duration for containers with timestamps
    if (fr_forced ||
        (frame->duration > 0 && !ts_unreliable && !duration_unreliable))
        return frame->duration;

    if (dp->dec_ctx->framerate.den && dp->dec_ctx->framerate.num) {
        int fields = frame->repeat_pict + 2;
        AVRational field_rate = av_mul_q(dp->dec_ctx->framerate,
                                         (AVRational){ 2, 1 });
        codec_duration = av_rescale_q(fields, av_inv_q(field_rate),
                                      frame->time_base);
    }

    // prefer codec-layer duration for containers without timestamps
    if (codec_duration > 0 && ts_unreliable)
        return codec_duration;

    // when timestamps are available, repeat last frame's actual duration
    // (i.e. pts difference between this and last frame)
    if (ts_diff > 0)
        return ts_diff;

    // try frame/codec duration
    if (frame->duration > 0)
        return frame->duration;
    if (codec_duration > 0)
        return codec_duration;

    // try average framerate
    if (dp->framerate_in.num && dp->framerate_in.den) {
        int64_t d = av_rescale_q(1, av_inv_q(dp->framerate_in),
                                 frame->time_base);
        if (d > 0)
            return d;
    }

    // last resort is last frame's estimated duration, and 1
    return FFMAX(dp->last_frame_duration_est, 1);
}

static int hwaccel_retrieve_data(AVCodecContext *avctx, AVFrame *input)
{
    DecoderPriv *dp = avctx->opaque;
    AVFrame *output = NULL;
    enum AVPixelFormat output_format = dp->hwaccel_output_format;
    int err;

    if (input->format == output_format) {
        // Nothing to do.
        return 0;
    }

    output = av_frame_alloc();
    if (!output)
        return AVERROR(ENOMEM);

    output->format = output_format;

    err = av_hwframe_transfer_data(output, input, 0);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to transfer data to "
               "output frame: %d.\n", err);
        goto fail;
    }

    err = av_frame_copy_props(output, input);
    if (err < 0) {
        av_frame_unref(output);
        goto fail;
    }

    av_frame_unref(input);
    av_frame_move_ref(input, output);
    av_frame_free(&output);

    return 0;

fail:
    av_frame_free(&output);
    return err;
}

static int video_frame_process(DecoderPriv *dp, AVFrame *frame,
                               unsigned *outputs_mask)
{
#if FFMPEG_OPT_TOP
    if (dp->flags & DECODER_FLAG_TOP_FIELD_FIRST) {
        av_log(dp, AV_LOG_WARNING, "-top is deprecated, use the setfield filter instead\n");
        frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
#endif

    if (frame->format == dp->hwaccel_pix_fmt) {
        int err = hwaccel_retrieve_data(dp->dec_ctx, frame);
        if (err < 0)
            return err;
    }

    frame->pts = frame->best_effort_timestamp;

    // forced fixed framerate
    if (dp->flags & DECODER_FLAG_FRAMERATE_FORCED) {
        frame->pts       = AV_NOPTS_VALUE;
        frame->duration  = 1;
        frame->time_base = av_inv_q(dp->framerate_in);
    }

    // no timestamp available - extrapolate from previous frame duration
    if (frame->pts == AV_NOPTS_VALUE)
        frame->pts = dp->last_frame_pts == AV_NOPTS_VALUE ? 0 :
                     dp->last_frame_pts + dp->last_frame_duration_est;

    // update timestamp history
    dp->last_frame_duration_est = video_duration_estimate(dp, frame);
    dp->last_frame_pts          = frame->pts;
    dp->last_frame_tb           = frame->time_base;

    if (debug_ts) {
        av_log(dp, AV_LOG_INFO,
               "decoder -> pts:%s pts_time:%s "
               "pkt_dts:%s pkt_dts_time:%s "
               "duration:%s duration_time:%s "
               "keyframe:%d frame_type:%d time_base:%d/%d\n",
               av_ts2str(frame->pts),
               av_ts2timestr(frame->pts, &frame->time_base),
               av_ts2str(frame->pkt_dts),
               av_ts2timestr(frame->pkt_dts, &frame->time_base),
               av_ts2str(frame->duration),
               av_ts2timestr(frame->duration, &frame->time_base),
               !!(frame->flags & AV_FRAME_FLAG_KEY), frame->pict_type,
               frame->time_base.num, frame->time_base.den);
    }

    if (dp->sar_override.num)
        frame->sample_aspect_ratio = dp->sar_override;

    if (dp->apply_cropping) {
        // lavfi does not require aligned frame data
        int ret = av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED);
        if (ret < 0) {
            av_log(dp, AV_LOG_ERROR, "Error applying decoder cropping\n");
            return ret;
        }
    }

    if (frame->opaque)
        *outputs_mask = (uintptr_t)frame->opaque;

    return 0;
}

static int copy_av_subtitle(AVSubtitle *dst, const AVSubtitle *src)
{
    int ret = AVERROR_BUG;
    AVSubtitle tmp = {
        .format = src->format,
        .start_display_time = src->start_display_time,
        .end_display_time = src->end_display_time,
        .num_rects = 0,
        .rects = NULL,
        .pts = src->pts
    };

    if (!src->num_rects)
        goto success;

    if (!(tmp.rects = av_calloc(src->num_rects, sizeof(*tmp.rects))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < src->num_rects; i++) {
        AVSubtitleRect *src_rect = src->rects[i];
        AVSubtitleRect *dst_rect;

        if (!(dst_rect = tmp.rects[i] = av_mallocz(sizeof(*tmp.rects[0])))) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        tmp.num_rects++;

        dst_rect->type      = src_rect->type;
        dst_rect->flags     = src_rect->flags;

        dst_rect->x         = src_rect->x;
        dst_rect->y         = src_rect->y;
        dst_rect->w         = src_rect->w;
        dst_rect->h         = src_rect->h;
        dst_rect->nb_colors = src_rect->nb_colors;

        if (src_rect->text)
            if (!(dst_rect->text = av_strdup(src_rect->text))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }

        if (src_rect->ass)
            if (!(dst_rect->ass = av_strdup(src_rect->ass))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }

        for (int j = 0; j < 4; j++) {
            // SUBTITLE_BITMAP images are special in the sense that they
            // are like PAL8 images. first pointer to data, second to
            // palette. This makes the size calculation match this.
            size_t buf_size = src_rect->type == SUBTITLE_BITMAP && j == 1 ?
                              AVPALETTE_SIZE :
                              src_rect->h * src_rect->linesize[j];

            if (!src_rect->data[j])
                continue;

            if (!(dst_rect->data[j] = av_memdup(src_rect->data[j], buf_size))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            dst_rect->linesize[j] = src_rect->linesize[j];
        }
    }

success:
    *dst = tmp;

    return 0;

cleanup:
    avsubtitle_free(&tmp);

    return ret;
}

static void subtitle_free(void *opaque, uint8_t *data)
{
    AVSubtitle *sub = (AVSubtitle*)data;
    avsubtitle_free(sub);
    av_free(sub);
}

static int subtitle_wrap_frame(AVFrame *frame, AVSubtitle *subtitle, int copy)
{
    AVBufferRef *buf;
    AVSubtitle *sub;
    int ret;

    if (copy) {
        sub = av_mallocz(sizeof(*sub));
        ret = sub ? copy_av_subtitle(sub, subtitle) : AVERROR(ENOMEM);
        if (ret < 0) {
            av_freep(&sub);
            return ret;
        }
    } else {
        sub = av_memdup(subtitle, sizeof(*subtitle));
        if (!sub)
            return AVERROR(ENOMEM);
        memset(subtitle, 0, sizeof(*subtitle));
    }

    buf = av_buffer_create((uint8_t*)sub, sizeof(*sub),
                           subtitle_free, NULL, 0);
    if (!buf) {
        avsubtitle_free(sub);
        av_freep(&sub);
        return AVERROR(ENOMEM);
    }

    frame->buf[0] = buf;

    return 0;
}

static int process_subtitle(DecoderPriv *dp, AVFrame *frame)
{
    const AVSubtitle *subtitle = (AVSubtitle*)frame->buf[0]->data;
    int ret = 0;

    if (dp->flags & DECODER_FLAG_FIX_SUB_DURATION) {
        AVSubtitle *sub_prev = dp->sub_prev[0]->buf[0] ?
                               (AVSubtitle*)dp->sub_prev[0]->buf[0]->data : NULL;
        int end = 1;
        if (sub_prev) {
            end = av_rescale(subtitle->pts - sub_prev->pts,
                             1000, AV_TIME_BASE);
            if (end < sub_prev->end_display_time) {
                av_log(dp, AV_LOG_DEBUG,
                       "Subtitle duration reduced from %"PRId32" to %d%s\n",
                       sub_prev->end_display_time, end,
                       end <= 0 ? ", dropping it" : "");
                sub_prev->end_display_time = end;
            }
        }

        av_frame_unref(dp->sub_prev[1]);
        av_frame_move_ref(dp->sub_prev[1], frame);

        frame    = dp->sub_prev[0];
        subtitle = frame->buf[0] ? (AVSubtitle*)frame->buf[0]->data : NULL;

        FFSWAP(AVFrame*, dp->sub_prev[0], dp->sub_prev[1]);

        if (end <= 0)
            return 0;
    }

    if (!subtitle)
        return 0;

    ret = sch_dec_send(dp->sch, dp->sch_idx, 0, frame);
    if (ret < 0)
        av_frame_unref(frame);

    return ret == AVERROR_EOF ? AVERROR_EXIT : ret;
}

static int fix_sub_duration_heartbeat(DecoderPriv *dp, int64_t signal_pts)
{
    int ret = AVERROR_BUG;
    AVSubtitle *prev_subtitle = dp->sub_prev[0]->buf[0] ?
        (AVSubtitle*)dp->sub_prev[0]->buf[0]->data : NULL;
    AVSubtitle *subtitle;

    if (!(dp->flags & DECODER_FLAG_FIX_SUB_DURATION) || !prev_subtitle ||
        !prev_subtitle->num_rects || signal_pts <= prev_subtitle->pts)
        return 0;

    av_frame_unref(dp->sub_heartbeat);
    ret = subtitle_wrap_frame(dp->sub_heartbeat, prev_subtitle, 1);
    if (ret < 0)
        return ret;

    subtitle = (AVSubtitle*)dp->sub_heartbeat->buf[0]->data;
    subtitle->pts = signal_pts;

    return process_subtitle(dp, dp->sub_heartbeat);
}

static int transcode_subtitles(DecoderPriv *dp, const AVPacket *pkt,
                               AVFrame *frame)
{
    AVPacket *flush_pkt = NULL;
    AVSubtitle subtitle;
    int got_output;
    int ret;

    if (pkt && (intptr_t)pkt->opaque == PKT_OPAQUE_SUB_HEARTBEAT) {
        frame->pts       = pkt->pts;
        frame->time_base = pkt->time_base;
        frame->opaque    = (void*)(intptr_t)FRAME_OPAQUE_SUB_HEARTBEAT;

        ret = sch_dec_send(dp->sch, dp->sch_idx, 0, frame);
        return ret == AVERROR_EOF ? AVERROR_EXIT : ret;
    } else if (pkt && (intptr_t)pkt->opaque == PKT_OPAQUE_FIX_SUB_DURATION) {
        return fix_sub_duration_heartbeat(dp, av_rescale_q(pkt->pts, pkt->time_base,
                                                           AV_TIME_BASE_Q));
    }

    if (!pkt) {
        flush_pkt = av_packet_alloc();
        if (!flush_pkt)
            return AVERROR(ENOMEM);
    }

    ret = avcodec_decode_subtitle2(dp->dec_ctx, &subtitle, &got_output,
                                   pkt ? pkt : flush_pkt);
    av_packet_free(&flush_pkt);

    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR, "Error decoding subtitles: %s\n",
               av_err2str(ret));
        dp->dec.decode_errors++;
        return exit_on_error ? ret : 0;
    }

    if (!got_output)
        return pkt ? 0 : AVERROR_EOF;

    dp->dec.frames_decoded++;

    // XXX the queue for transferring data to consumers runs
    // on AVFrames, so we wrap AVSubtitle in an AVBufferRef and put that
    // inside the frame
    // eventually, subtitles should be switched to use AVFrames natively
    ret = subtitle_wrap_frame(frame, &subtitle, 0);
    if (ret < 0) {
        avsubtitle_free(&subtitle);
        return ret;
    }

    frame->width  = dp->dec_ctx->width;
    frame->height = dp->dec_ctx->height;

    return process_subtitle(dp, frame);
}

static int packet_decode(DecoderPriv *dp, AVPacket *pkt, AVFrame *frame)
{
    AVCodecContext *dec = dp->dec_ctx;
    const char *type_desc = av_get_media_type_string(dec->codec_type);
    int ret;

    if (dec->codec_type == AVMEDIA_TYPE_SUBTITLE)
        return transcode_subtitles(dp, pkt, frame);

    // With fate-indeo3-2, we're getting 0-sized packets before EOF for some
    // reason. This seems like a semi-critical bug. Don't trigger EOF, and
    // skip the packet.
    if (pkt && pkt->size == 0)
        return 0;

    if (pkt && (dp->flags & DECODER_FLAG_TS_UNRELIABLE)) {
        pkt->pts = AV_NOPTS_VALUE;
        pkt->dts = AV_NOPTS_VALUE;
    }

    if (pkt) {
        FrameData *fd = packet_data(pkt);
        if (!fd)
            return AVERROR(ENOMEM);
        fd->wallclock[LATENCY_PROBE_DEC_PRE] = av_gettime_relative();
    }

    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && !(ret == AVERROR_EOF && !pkt)) {
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret == AVERROR(EAGAIN)) {
            av_log(dp, AV_LOG_FATAL, "A decoder returned an unexpected error code. "
                                     "This is a bug, please report it.\n");
            return AVERROR_BUG;
        }
        av_log(dp, AV_LOG_ERROR, "Error submitting %s to decoder: %s\n",
               pkt ? "packet" : "EOF", av_err2str(ret));

        if (ret == AVERROR_EOF)
            return ret;

        dp->dec.decode_errors++;
        if (exit_on_error)
            return ret;
    }

    while (1) {
        FrameData *fd;
        unsigned outputs_mask = 1;
        unsigned flags = 0;
        if (!dp->dec.frames_decoded)
            flags |= AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS;

        av_frame_unref(frame);

        update_benchmark(NULL);
        ret = avcodec_receive_frame_flags(dec, frame, flags);
        update_benchmark("decode_%s %s", type_desc, dp->parent_name);

        if (ret == AVERROR(EAGAIN)) {
            av_assert0(pkt); // should never happen during flushing
            return 0;
        } else if (ret == AVERROR_EOF) {
            return ret;
        } else if (ret < 0) {
            av_log(dp, AV_LOG_ERROR, "Decoding error: %s\n", av_err2str(ret));
            dp->dec.decode_errors++;

            if (exit_on_error)
                return ret;

            continue;
        }

        if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
            av_log(dp, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "corrupt decoded frame\n");
            if (exit_on_error)
                return AVERROR_INVALIDDATA;
        }

        fd      = frame_data(frame);
        if (!fd) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        fd->dec.pts                 = frame->pts;
        fd->dec.tb                  = dec->pkt_timebase;
        fd->dec.frame_num           = dec->frame_num - 1;
        fd->bits_per_raw_sample     = dec->bits_per_raw_sample;

        fd->wallclock[LATENCY_PROBE_DEC_POST] = av_gettime_relative();

        frame->time_base = dec->pkt_timebase;

        if (dec->codec_type == AVMEDIA_TYPE_AUDIO) {
            dp->dec.samples_decoded += frame->nb_samples;

            audio_ts_process(dp, frame);
        } else {
            ret = video_frame_process(dp, frame, &outputs_mask);
            if (ret < 0) {
                av_log(dp, AV_LOG_FATAL,
                       "Error while processing the decoded data\n");
                return ret;
            }
        }

        dp->dec.frames_decoded++;

        for (int i = 0; i < stdc_count_ones(outputs_mask); i++) {
            AVFrame *to_send = frame;
            int pos;

            av_assert0(outputs_mask);
            pos = stdc_trailing_zeros(outputs_mask);
            outputs_mask &= ~(1U << pos);

            // this is not the last output and sch_dec_send() consumes the frame
            // given to it, so make a temporary reference
            if (outputs_mask) {
                to_send = dp->frame_tmp_ref;
                ret = av_frame_ref(to_send, frame);
                if (ret < 0)
                    return ret;
            }

            ret = sch_dec_send(dp->sch, dp->sch_idx, pos, to_send);
            if (ret < 0) {
                av_frame_unref(to_send);
                return ret == AVERROR_EOF ? AVERROR_EXIT : ret;
            }
        }
    }
}

static int dec_open(DecoderPriv *dp, AVDictionary **dec_opts,
                    const DecoderOpts *o, AVFrame *param_out);

static int dec_standalone_open(DecoderPriv *dp, const AVPacket *pkt)
{
    DecoderOpts o;
    const FrameData *fd;
    char name[16];

    if (!pkt->opaque_ref)
        return AVERROR_BUG;
    fd = (FrameData *)pkt->opaque_ref->data;

    if (!fd->par_enc)
        return AVERROR_BUG;

    memset(&o, 0, sizeof(o));

    o.par       = fd->par_enc;
    o.time_base = pkt->time_base;

    o.codec = dp->standalone_init.codec;
    if (!o.codec)
        o.codec = avcodec_find_decoder(o.par->codec_id);
    if (!o.codec) {
        const AVCodecDescriptor *desc = avcodec_descriptor_get(o.par->codec_id);

        av_log(dp, AV_LOG_ERROR, "Cannot find a decoder for codec ID '%s'\n",
               desc ? desc->name : "?");
        return AVERROR_DECODER_NOT_FOUND;
    }

    snprintf(name, sizeof(name), "dec%d", dp->index);
    o.name = name;

    return dec_open(dp, &dp->standalone_init.opts, &o, NULL);
}

static void dec_thread_set_name(const DecoderPriv *dp)
{
    char name[16] = "dec";

    if (dp->index >= 0)
        av_strlcatf(name, sizeof(name), "%d", dp->index);
    else if (dp->parent_name)
        av_strlcat(name, dp->parent_name, sizeof(name));

    if (dp->dec_ctx)
        av_strlcatf(name, sizeof(name), ":%s", dp->dec_ctx->codec->name);

    ff_thread_setname(name);
}

static void dec_thread_uninit(DecThreadContext *dt)
{
    av_packet_free(&dt->pkt);
    av_frame_free(&dt->frame);

    memset(dt, 0, sizeof(*dt));
}

static int dec_thread_init(DecThreadContext *dt)
{
    memset(dt, 0, sizeof(*dt));

    dt->frame = av_frame_alloc();
    if (!dt->frame)
        goto fail;

    dt->pkt = av_packet_alloc();
    if (!dt->pkt)
        goto fail;

    return 0;

fail:
    dec_thread_uninit(dt);
    return AVERROR(ENOMEM);
}

static int decoder_thread(void *arg)
{
    DecoderPriv  *dp = arg;
    DecThreadContext dt;
    int ret = 0, input_status = 0;

    ret = dec_thread_init(&dt);
    if (ret < 0)
        goto finish;

    dec_thread_set_name(dp);

    while (!input_status) {
        int flush_buffers, have_data;

        input_status  = sch_dec_receive(dp->sch, dp->sch_idx, dt.pkt);
        have_data     = input_status >= 0 &&
            (dt.pkt->buf || dt.pkt->side_data_elems ||
             (intptr_t)dt.pkt->opaque == PKT_OPAQUE_SUB_HEARTBEAT ||
             (intptr_t)dt.pkt->opaque == PKT_OPAQUE_FIX_SUB_DURATION);
        flush_buffers = input_status >= 0 && !have_data;
        if (!have_data)
            av_log(dp, AV_LOG_VERBOSE, "Decoder thread received %s packet\n",
                   flush_buffers ? "flush" : "EOF");

        // this is a standalone decoder that has not been initialized yet
        if (!dp->dec_ctx) {
            if (flush_buffers)
                continue;
            if (input_status < 0) {
                av_log(dp, AV_LOG_ERROR,
                       "Cannot initialize a standalone decoder\n");
                ret = input_status;
                goto finish;
            }

            ret = dec_standalone_open(dp, dt.pkt);
            if (ret < 0)
                goto finish;
        }

        ret = packet_decode(dp, have_data ? dt.pkt : NULL, dt.frame);

        av_packet_unref(dt.pkt);
        av_frame_unref(dt.frame);

        // AVERROR_EOF  - EOF from the decoder
        // AVERROR_EXIT - EOF from the scheduler
        // we treat them differently when flushing
        if (ret == AVERROR_EXIT) {
            ret = AVERROR_EOF;
            flush_buffers = 0;
        }

        if (ret == AVERROR_EOF) {
            av_log(dp, AV_LOG_VERBOSE, "Decoder returned EOF, %s\n",
                   flush_buffers ? "resetting" : "finishing");

            if (!flush_buffers)
                break;

            /* report last frame duration to the scheduler */
            if (dp->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                dt.pkt->pts       = dp->last_frame_pts + dp->last_frame_duration_est;
                dt.pkt->time_base = dp->last_frame_tb;
            }

            avcodec_flush_buffers(dp->dec_ctx);
        } else if (ret < 0) {
            av_log(dp, AV_LOG_ERROR, "Error processing packet in decoder: %s\n",
                   av_err2str(ret));
            break;
        }
    }

    // EOF is normal thread termination
    if (ret == AVERROR_EOF)
        ret = 0;

    // on success send EOF timestamp to our downstreams
    if (ret >= 0) {
        float err_rate;

        av_frame_unref(dt.frame);

        dt.frame->opaque    = (void*)(intptr_t)FRAME_OPAQUE_EOF;
        dt.frame->pts       = dp->last_frame_pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                              dp->last_frame_pts + dp->last_frame_duration_est;
        dt.frame->time_base = dp->last_frame_tb;

        ret = sch_dec_send(dp->sch, dp->sch_idx, 0, dt.frame);
        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(dp, AV_LOG_FATAL,
                   "Error signalling EOF timestamp: %s\n", av_err2str(ret));
            goto finish;
        }
        ret = 0;

        err_rate = (dp->dec.frames_decoded || dp->dec.decode_errors) ?
                   (float)dp->dec.decode_errors / (dp->dec.frames_decoded + dp->dec.decode_errors) : 0.f;
        if (err_rate > max_error_rate) {
            av_log(dp, AV_LOG_FATAL, "Decode error rate %g exceeds maximum %g\n",
                   err_rate, max_error_rate);
            ret = FFMPEG_ERROR_RATE_EXCEEDED;
        } else if (err_rate)
            av_log(dp, AV_LOG_VERBOSE, "Decode error rate %g\n", err_rate);
    }

finish:
    dec_thread_uninit(&dt);
    avcodec_free_context(&dp->dec_ctx);

    return ret;
}

int dec_request_view(Decoder *d, const ViewSpecifier *vs,
                     SchedulerNode *src)
{
    DecoderPriv *dp = dp_from_dec(d);
    unsigned out_idx = 0;
    int ret;

    if (dp->multiview_user_config) {
        if (!vs || vs->type == VIEW_SPECIFIER_TYPE_NONE) {
            *src = SCH_DEC_OUT(dp->sch_idx, 0);
            return 0;
        }

        av_log(dp, AV_LOG_ERROR,
               "Manually selecting views with -view_ids cannot be combined "
               "with view selection via stream specifiers. It is strongly "
               "recommended you always use stream specifiers only.\n");
        return AVERROR(EINVAL);
    }

    // when multiview_user_config is not set, NONE specifier is treated
    // as requesting the base view
    vs = (vs && vs->type != VIEW_SPECIFIER_TYPE_NONE) ? vs :
         &(ViewSpecifier){ .type = VIEW_SPECIFIER_TYPE_IDX, .val = 0 };

    // check if the specifier matches an already-existing one
    for (int i = 0; i < dp->nb_views_requested; i++) {
        const ViewSpecifier *vs1 = &dp->views_requested[i].vs;

        if (vs->type == vs1->type &&
            (vs->type == VIEW_SPECIFIER_TYPE_ALL || vs->val == vs1->val)) {
            *src = SCH_DEC_OUT(dp->sch_idx, dp->views_requested[i].out_idx);
            return 0;
        }
    }

    // we use a bitmask to map view IDs to decoder outputs, which
    // limits the number of outputs allowed
    if (dp->nb_views_requested >= sizeof(dp->view_map[0].out_mask) * 8) {
        av_log(dp, AV_LOG_ERROR, "Too many view specifiers\n");
        return AVERROR(ENOSYS);
    }

    ret = GROW_ARRAY(dp->views_requested, dp->nb_views_requested);
    if (ret < 0)
        return ret;

    if (dp->nb_views_requested > 1) {
        ret = sch_add_dec_output(dp->sch, dp->sch_idx);
        if (ret < 0)
            return ret;
        out_idx = ret;
    }

    dp->views_requested[dp->nb_views_requested - 1].out_idx = out_idx;
    dp->views_requested[dp->nb_views_requested - 1].vs      = *vs;

    *src = SCH_DEC_OUT(dp->sch_idx,
                       dp->views_requested[dp->nb_views_requested - 1].out_idx);

    return 0;
}

static int multiview_setup(DecoderPriv *dp, AVCodecContext *dec_ctx)
{
    unsigned views_wanted = 0;

    unsigned nb_view_ids_av, nb_view_ids;
    unsigned *view_ids_av = NULL, *view_pos_av = NULL;
    int      *view_ids    = NULL;
    int ret;

    // no views/only base view were requested - do nothing
    if (!dp->nb_views_requested ||
        (dp->nb_views_requested == 1                               &&
         dp->views_requested[0].vs.type == VIEW_SPECIFIER_TYPE_IDX &&
         dp->views_requested[0].vs.val  == 0))
        return 0;

    av_freep(&dp->view_map);
    dp->nb_view_map = 0;

    // retrieve views available in current CVS
    ret = av_opt_get_array_size(dec_ctx, "view_ids_available",
                                AV_OPT_SEARCH_CHILDREN, &nb_view_ids_av);
    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR,
               "Multiview decoding requested, but decoder '%s' does not "
               "support it\n", dec_ctx->codec->name);
        return AVERROR(ENOSYS);
    }

    if (nb_view_ids_av) {
        unsigned nb_view_pos_av;

        if (nb_view_ids_av >= sizeof(views_wanted) * 8) {
            av_log(dp, AV_LOG_ERROR, "Too many views in video: %u\n", nb_view_ids_av);
            ret = AVERROR(ENOSYS);
            goto fail;
        }

        view_ids_av = av_calloc(nb_view_ids_av, sizeof(*view_ids_av));
        if (!view_ids_av) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = av_opt_get_array(dec_ctx, "view_ids_available",
                               AV_OPT_SEARCH_CHILDREN, 0, nb_view_ids_av,
                               AV_OPT_TYPE_UINT, view_ids_av);
        if (ret < 0)
            goto fail;

        ret = av_opt_get_array_size(dec_ctx, "view_pos_available",
                                    AV_OPT_SEARCH_CHILDREN, &nb_view_pos_av);
        if (ret >= 0 && nb_view_pos_av == nb_view_ids_av) {
            view_pos_av = av_calloc(nb_view_ids_av, sizeof(*view_pos_av));
            if (!view_pos_av) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            ret = av_opt_get_array(dec_ctx, "view_pos_available",
                                   AV_OPT_SEARCH_CHILDREN, 0, nb_view_ids_av,
                                   AV_OPT_TYPE_UINT, view_pos_av);
            if (ret < 0)
                goto fail;
        }
    } else {
        // assume there is a single view with ID=0
        nb_view_ids_av = 1;
        view_ids_av = av_calloc(nb_view_ids_av, sizeof(*view_ids_av));
        view_pos_av = av_calloc(nb_view_ids_av, sizeof(*view_pos_av));
        if (!view_ids_av || !view_pos_av) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        view_pos_av[0] = AV_STEREO3D_VIEW_UNSPEC;
    }

    dp->view_map = av_calloc(nb_view_ids_av, sizeof(*dp->view_map));
    if (!dp->view_map) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    dp->nb_view_map = nb_view_ids_av;

    for (int i = 0; i < dp->nb_view_map; i++)
        dp->view_map[i].id = view_ids_av[i];

    // figure out which views should go to which output
    for (int i = 0; i < dp->nb_views_requested; i++) {
        const ViewSpecifier *vs = &dp->views_requested[i].vs;

        switch (vs->type) {
        case VIEW_SPECIFIER_TYPE_IDX:
            if (vs->val >= nb_view_ids_av) {
                av_log(dp, exit_on_error ? AV_LOG_ERROR : AV_LOG_WARNING,
                       "View with index %u requested, but only %u views available "
                       "in current video sequence (more views may or may not be "
                       "available in later sequences).\n",
                       vs->val, nb_view_ids_av);
                if (exit_on_error) {
                    ret = AVERROR(EINVAL);
                    goto fail;
                }

                continue;
            }
            views_wanted                   |= 1U   << vs->val;
            dp->view_map[vs->val].out_mask |= 1ULL << i;

            break;
        case VIEW_SPECIFIER_TYPE_ID: {
            int view_idx = -1;

            for (unsigned j = 0; j < nb_view_ids_av; j++) {
                if (view_ids_av[j] == vs->val) {
                    view_idx = j;
                    break;
                }
            }
            if (view_idx < 0) {
                av_log(dp, exit_on_error ? AV_LOG_ERROR : AV_LOG_WARNING,
                       "View with ID %u requested, but is not available "
                       "in the video sequence\n", vs->val);
                if (exit_on_error) {
                    ret = AVERROR(EINVAL);
                    goto fail;
                }

                continue;
            }
            views_wanted                    |= 1U   << view_idx;
            dp->view_map[view_idx].out_mask |= 1ULL << i;

            break;
            }
        case VIEW_SPECIFIER_TYPE_POS: {
            int view_idx = -1;

            for (unsigned j = 0; view_pos_av && j < nb_view_ids_av; j++) {
                if (view_pos_av[j] == vs->val) {
                    view_idx = j;
                    break;
                }
            }
            if (view_idx < 0) {
                av_log(dp, exit_on_error ? AV_LOG_ERROR : AV_LOG_WARNING,
                       "View position '%s' requested, but is not available "
                       "in the video sequence\n", av_stereo3d_view_name(vs->val));
                if (exit_on_error) {
                    ret = AVERROR(EINVAL);
                    goto fail;
                }

                continue;
            }
            views_wanted                    |= 1U   << view_idx;
            dp->view_map[view_idx].out_mask |= 1ULL << i;

            break;
            }
        case VIEW_SPECIFIER_TYPE_ALL:
            views_wanted |= (1U << nb_view_ids_av) - 1;

            for (int j = 0; j < dp->nb_view_map; j++)
                dp->view_map[j].out_mask |= 1ULL << i;

            break;
        }
    }
    if (!views_wanted) {
        av_log(dp, AV_LOG_ERROR, "No views were selected for decoding\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    // signal to decoder which views we want
    nb_view_ids = stdc_count_ones(views_wanted);
    view_ids = av_malloc_array(nb_view_ids, sizeof(*view_ids));
    if (!view_ids) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (unsigned i = 0; i < nb_view_ids; i++) {
        int pos;

        av_assert0(views_wanted);
        pos = stdc_trailing_zeros(views_wanted);
        views_wanted &= ~(1U << pos);

        view_ids[i] = view_ids_av[pos];
    }

    // unset view_ids in case we set it earlier
    av_opt_set(dec_ctx, "view_ids", NULL, AV_OPT_SEARCH_CHILDREN);

    ret = av_opt_set_array(dec_ctx, "view_ids", AV_OPT_SEARCH_CHILDREN,
                           0, nb_view_ids, AV_OPT_TYPE_INT, view_ids);
    if (ret < 0)
        goto fail;

    if (!dp->frame_tmp_ref) {
        dp->frame_tmp_ref = av_frame_alloc();
        if (!dp->frame_tmp_ref) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

fail:
    av_freep(&view_ids_av);
    av_freep(&view_pos_av);
    av_freep(&view_ids);

    return ret;
}

static void multiview_check_manual(DecoderPriv *dp, const AVDictionary *dec_opts)
{
    if (av_dict_get(dec_opts, "view_ids", NULL, 0)) {
        av_log(dp, AV_LOG_WARNING, "Manually selecting views with -view_ids "
               "is not recommended, use view specifiers instead\n");
        dp->multiview_user_config = 1;
    }
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    DecoderPriv  *dp = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    ret = multiview_setup(dp, s);
    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR, "Error setting up multiview decoding: %s\n",
               av_err2str(ret));
        return AV_PIX_FMT_NONE;
    }

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const AVCodecHWConfig  *config = NULL;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (dp->hwaccel_id == HWACCEL_GENERIC ||
            dp->hwaccel_id == HWACCEL_AUTO) {
            for (int i = 0;; i++) {
                config = avcodec_get_hw_config(s->codec, i);
                if (!config)
                    break;
                if (!(config->methods &
                      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                if (config->pix_fmt == *p)
                    break;
            }
        }
        if (config && config->device_type == dp->hwaccel_device_type) {
            dp->hwaccel_pix_fmt = *p;
            break;
        }
    }

    return *p;
}

static int get_buffer(AVCodecContext *dec_ctx, AVFrame *frame, int flags)
{
    DecoderPriv *dp = dec_ctx->opaque;

    // for multiview video, store the output mask in frame opaque
    if (dp->nb_view_map) {
        const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);
        int view_id = sd ? *(int*)sd->data : 0;

        for (int i = 0; i < dp->nb_view_map; i++) {
            if (dp->view_map[i].id == view_id) {
                frame->opaque = (void*)dp->view_map[i].out_mask;
                break;
            }
        }
    }

    return avcodec_default_get_buffer2(dec_ctx, frame, flags);
}

static HWDevice *hw_device_match_by_codec(const AVCodec *codec)
{
    const AVCodecHWConfig *config;
    HWDevice *dev;
    for (int i = 0;; i++) {
        config = avcodec_get_hw_config(codec, i);
        if (!config)
            return NULL;
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            continue;
        dev = hw_device_get_by_type(config->device_type);
        if (dev)
            return dev;
    }
}

static int hw_device_setup_for_decode(DecoderPriv *dp,
                                      const AVCodec *codec,
                                      const char *hwaccel_device)
{
    const AVCodecHWConfig *config;
    enum AVHWDeviceType type;
    HWDevice *dev = NULL;
    int err, auto_device = 0;

    if (hwaccel_device) {
        dev = hw_device_get_by_name(hwaccel_device);
        if (!dev) {
            if (dp->hwaccel_id == HWACCEL_AUTO) {
                auto_device = 1;
            } else if (dp->hwaccel_id == HWACCEL_GENERIC) {
                type = dp->hwaccel_device_type;
                err = hw_device_init_from_type(type, hwaccel_device,
                                               &dev);
            } else {
                // This will be dealt with by API-specific initialisation
                // (using hwaccel_device), so nothing further needed here.
                return 0;
            }
        } else {
            if (dp->hwaccel_id == HWACCEL_AUTO) {
                dp->hwaccel_device_type = dev->type;
            } else if (dp->hwaccel_device_type != dev->type) {
                av_log(dp, AV_LOG_ERROR, "Invalid hwaccel device "
                       "specified for decoder: device %s of type %s is not "
                       "usable with hwaccel %s.\n", dev->name,
                       av_hwdevice_get_type_name(dev->type),
                       av_hwdevice_get_type_name(dp->hwaccel_device_type));
                return AVERROR(EINVAL);
            }
        }
    } else {
        if (dp->hwaccel_id == HWACCEL_AUTO) {
            auto_device = 1;
        } else if (dp->hwaccel_id == HWACCEL_GENERIC) {
            type = dp->hwaccel_device_type;
            dev = hw_device_get_by_type(type);

            // When "-qsv_device device" is used, an internal QSV device named
            // as "__qsv_device" is created. Another QSV device is created too
            // if "-init_hw_device qsv=name:device" is used. There are 2 QSV devices
            // if both "-qsv_device device" and "-init_hw_device qsv=name:device"
            // are used, hw_device_get_by_type(AV_HWDEVICE_TYPE_QSV) returns NULL.
            // To keep back-compatibility with the removed ad-hoc libmfx setup code,
            // call hw_device_get_by_name("__qsv_device") to select the internal QSV
            // device.
            if (!dev && type == AV_HWDEVICE_TYPE_QSV)
                dev = hw_device_get_by_name("__qsv_device");

            if (!dev)
                err = hw_device_init_from_type(type, NULL, &dev);
        } else {
            dev = hw_device_match_by_codec(codec);
            if (!dev) {
                // No device for this codec, but not using generic hwaccel
                // and therefore may well not need one - ignore.
                return 0;
            }
        }
    }

    if (auto_device) {
        if (!avcodec_get_hw_config(codec, 0)) {
            // Decoder does not support any hardware devices.
            return 0;
        }
        for (int i = 0; !dev; i++) {
            config = avcodec_get_hw_config(codec, i);
            if (!config)
                break;
            type = config->device_type;
            dev = hw_device_get_by_type(type);
            if (dev) {
                av_log(dp, AV_LOG_INFO, "Using auto "
                       "hwaccel type %s with existing device %s.\n",
                       av_hwdevice_get_type_name(type), dev->name);
            }
        }
        for (int i = 0; !dev; i++) {
            config = avcodec_get_hw_config(codec, i);
            if (!config)
                break;
            type = config->device_type;
            // Try to make a new device of this type.
            err = hw_device_init_from_type(type, hwaccel_device,
                                           &dev);
            if (err < 0) {
                // Can't make a device of this type.
                continue;
            }
            if (hwaccel_device) {
                av_log(dp, AV_LOG_INFO, "Using auto "
                       "hwaccel type %s with new device created "
                       "from %s.\n", av_hwdevice_get_type_name(type),
                       hwaccel_device);
            } else {
                av_log(dp, AV_LOG_INFO, "Using auto "
                       "hwaccel type %s with new default device.\n",
                       av_hwdevice_get_type_name(type));
            }
        }
        if (dev) {
            dp->hwaccel_device_type = type;
        } else {
            av_log(dp, AV_LOG_INFO, "Auto hwaccel "
                   "disabled: no device found.\n");
            dp->hwaccel_id = HWACCEL_NONE;
            return 0;
        }
    }

    if (!dev) {
        av_log(dp, AV_LOG_ERROR, "No device available "
               "for decoder: device type %s needed for codec %s.\n",
               av_hwdevice_get_type_name(type), codec->name);
        return err;
    }

    dp->dec_ctx->hw_device_ctx = av_buffer_ref(dev->device_ref);
    if (!dp->dec_ctx->hw_device_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int dec_open(DecoderPriv *dp, AVDictionary **dec_opts,
                    const DecoderOpts *o, AVFrame *param_out)
{
    const AVCodec *codec = o->codec;
    int ret;

    dp->flags      = o->flags;
    dp->log_parent = o->log_parent;

    dp->dec.type                = codec->type;
    dp->framerate_in            = o->framerate;

    dp->hwaccel_id              = o->hwaccel_id;
    dp->hwaccel_device_type     = o->hwaccel_device_type;
    dp->hwaccel_output_format   = o->hwaccel_output_format;

    snprintf(dp->log_name, sizeof(dp->log_name), "dec:%s", codec->name);

    dp->parent_name = av_strdup(o->name ? o->name : "");
    if (!dp->parent_name)
        return AVERROR(ENOMEM);

    if (codec->type == AVMEDIA_TYPE_SUBTITLE &&
        (dp->flags & DECODER_FLAG_FIX_SUB_DURATION)) {
        for (int i = 0; i < FF_ARRAY_ELEMS(dp->sub_prev); i++) {
            dp->sub_prev[i] = av_frame_alloc();
            if (!dp->sub_prev[i])
                return AVERROR(ENOMEM);
        }
        dp->sub_heartbeat = av_frame_alloc();
        if (!dp->sub_heartbeat)
            return AVERROR(ENOMEM);
    }

    dp->sar_override = o->par->sample_aspect_ratio;

    dp->dec_ctx = avcodec_alloc_context3(codec);
    if (!dp->dec_ctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(dp->dec_ctx, o->par);
    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR, "Error initializing the decoder context.\n");
        return ret;
    }

    dp->dec_ctx->opaque                = dp;
    dp->dec_ctx->get_format            = get_format;
    dp->dec_ctx->get_buffer2           = get_buffer;
    dp->dec_ctx->pkt_timebase          = o->time_base;

    if (!av_dict_get(*dec_opts, "threads", NULL, 0))
        av_dict_set(dec_opts, "threads", "auto", 0);

    ret = hw_device_setup_for_decode(dp, codec, o->hwaccel_device);
    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR,
               "Hardware device setup failed for decoder: %s\n",
               av_err2str(ret));
        return ret;
    }

    ret = av_opt_set_dict2(dp->dec_ctx, dec_opts, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(dp, AV_LOG_ERROR, "Error applying decoder options: %s\n",
               av_err2str(ret));
        return ret;
    }
    ret = check_avoptions(*dec_opts);
    if (ret < 0)
        return ret;

    dp->dec_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    if (o->flags & DECODER_FLAG_BITEXACT)
        dp->dec_ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    // we apply cropping ourselves
    dp->apply_cropping          = dp->dec_ctx->apply_cropping;
    dp->dec_ctx->apply_cropping = 0;

    if ((ret = avcodec_open2(dp->dec_ctx, codec, NULL)) < 0) {
        av_log(dp, AV_LOG_ERROR, "Error while opening decoder: %s\n",
               av_err2str(ret));
        return ret;
    }

    if (dp->dec_ctx->hw_device_ctx) {
        // Update decoder extra_hw_frames option to account for the
        // frames held in queues inside the ffmpeg utility.  This is
        // called after avcodec_open2() because the user-set value of
        // extra_hw_frames becomes valid in there, and we need to add
        // this on top of it.
        int extra_frames = DEFAULT_FRAME_THREAD_QUEUE_SIZE;
        if (dp->dec_ctx->extra_hw_frames >= 0)
            dp->dec_ctx->extra_hw_frames += extra_frames;
        else
            dp->dec_ctx->extra_hw_frames = extra_frames;
    }

    dp->dec.subtitle_header      = dp->dec_ctx->subtitle_header;
    dp->dec.subtitle_header_size = dp->dec_ctx->subtitle_header_size;

    if (param_out) {
        if (dp->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            param_out->format               = dp->dec_ctx->sample_fmt;
            param_out->sample_rate          = dp->dec_ctx->sample_rate;

            ret = av_channel_layout_copy(&param_out->ch_layout, &dp->dec_ctx->ch_layout);
            if (ret < 0)
                return ret;
        } else if (dp->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            param_out->format               = dp->dec_ctx->pix_fmt;
            param_out->width                = dp->dec_ctx->width;
            param_out->height               = dp->dec_ctx->height;
            param_out->sample_aspect_ratio  = dp->dec_ctx->sample_aspect_ratio;
            param_out->colorspace           = dp->dec_ctx->colorspace;
            param_out->color_range          = dp->dec_ctx->color_range;
            param_out->alpha_mode           = dp->dec_ctx->alpha_mode;
        }

        av_frame_side_data_free(&param_out->side_data, &param_out->nb_side_data);
        ret = clone_side_data(&param_out->side_data, &param_out->nb_side_data,
                              dp->dec_ctx->decoded_side_data, dp->dec_ctx->nb_decoded_side_data, 0);
        if (ret < 0)
            return ret;
        param_out->time_base = dp->dec_ctx->pkt_timebase;
    }

    return 0;
}

int dec_init(Decoder **pdec, Scheduler *sch,
             AVDictionary **dec_opts, const DecoderOpts *o,
             AVFrame *param_out)
{
    DecoderPriv *dp;
    int ret;

    *pdec = NULL;

    ret = dec_alloc(&dp, sch, !!(o->flags & DECODER_FLAG_SEND_END_TS));
    if (ret < 0)
        return ret;

    multiview_check_manual(dp, *dec_opts);

    ret = dec_open(dp, dec_opts, o, param_out);
    if (ret < 0)
        goto fail;

    *pdec = &dp->dec;

    return dp->sch_idx;
fail:
    dec_free((Decoder**)&dp);
    return ret;
}

int dec_create(const OptionsContext *o, const char *arg, Scheduler *sch)
{
    DecoderPriv *dp;

    OutputFile      *of;
    OutputStream    *ost;
    int of_index, ost_index;
    char *p;

    unsigned enc_idx;
    int ret;

    ret = dec_alloc(&dp, sch, 0);
    if (ret < 0)
        return ret;

    dp->index = nb_decoders;

    ret = GROW_ARRAY(decoders, nb_decoders);
    if (ret < 0) {
        dec_free((Decoder **)&dp);
        return ret;
    }

    decoders[nb_decoders - 1] = (Decoder *)dp;

    of_index = strtol(arg, &p, 0);
    if (of_index < 0 || of_index >= nb_output_files) {
        av_log(dp, AV_LOG_ERROR, "Invalid output file index '%d' in %s\n", of_index, arg);
        return AVERROR(EINVAL);
    }
    of = output_files[of_index];

    ost_index = strtol(p + 1, NULL, 0);
    if (ost_index < 0 || ost_index >= of->nb_streams) {
        av_log(dp, AV_LOG_ERROR, "Invalid output stream index '%d' in %s\n", ost_index, arg);
        return AVERROR(EINVAL);
    }
    ost = of->streams[ost_index];

    if (!ost->enc) {
        av_log(dp, AV_LOG_ERROR, "Output stream %s has no encoder\n", arg);
        return AVERROR(EINVAL);
    }

    dp->dec.type = ost->type;

    ret = enc_loopback(ost->enc);
    if (ret < 0)
        return ret;
    enc_idx = ret;

    ret = sch_connect(sch, SCH_ENC(enc_idx), SCH_DEC_IN(dp->sch_idx));
    if (ret < 0)
        return ret;

    ret = av_dict_copy(&dp->standalone_init.opts, o->g->codec_opts, 0);
    if (ret < 0)
        return ret;

    multiview_check_manual(dp, dp->standalone_init.opts);

    if (o->codec_names.nb_opt) {
        const char *name = o->codec_names.opt[o->codec_names.nb_opt - 1].u.str;
        dp->standalone_init.codec = avcodec_find_decoder_by_name(name);
        if (!dp->standalone_init.codec) {
            av_log(dp, AV_LOG_ERROR, "No such decoder: %s\n", name);
            return AVERROR_DECODER_NOT_FOUND;
        }
    }

    return 0;
}

int dec_filter_add(Decoder *d, InputFilter *ifilter, InputFilterOptions *opts,
                   const ViewSpecifier *vs, SchedulerNode *src)
{
    DecoderPriv *dp = dp_from_dec(d);
    char name[16];

    snprintf(name, sizeof(name), "dec%d", dp->index);
    opts->name = av_strdup(name);
    if (!opts->name)
        return AVERROR(ENOMEM);

    return dec_request_view(d, vs, src);
}


/* ========== fftools/ffmpeg_demux.c ========== */

/*
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

#include <float.h>
#include <stdint.h>

#include "ffmpeg.h"
#include "ffmpeg_sched.h"
#include "ffmpeg_utils.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/display.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/bsf.h"
#include "libavcodec/packet.h"

#include "libavformat/avformat.h"

typedef struct DemuxStream {
    InputStream              ist;

    // name used for logging
    char                     log_name[32];

    int                      sch_idx_stream;
    int                      sch_idx_dec;

    double                   ts_scale;

    /* non zero if the packets must be decoded in 'raw_fifo', see DECODING_FOR_* */
    int                      decoding_needed;
#define DECODING_FOR_OST    1
#define DECODING_FOR_FILTER 2

    /* true if stream data should be discarded */
    int                      discard;

    // scheduler returned EOF for this stream
    int                      finished;

    int                      streamcopy_needed;
    int                      have_sub2video;
    int                      reinit_filters;
    int                      autorotate;
    int                      apply_cropping;
    int                      force_display_matrix;
    int                      drop_changed;


    int                      wrap_correction_done;
    int                      saw_first_ts;
    /// dts of the first packet read for this stream (in AV_TIME_BASE units)
    int64_t                  first_dts;

    /* predicted dts of the next packet read for this stream or (when there are
     * several frames in a packet) of the next frame in current packet (in AV_TIME_BASE units) */
    int64_t                  next_dts;
    /// dts of the last packet read for this stream (in AV_TIME_BASE units)
    int64_t                  dts;

    const AVCodecDescriptor *codec_desc;

    AVDictionary            *decoder_opts;
    DecoderOpts              dec_opts;
    char                     dec_name[16];
    // decoded media properties, as estimated by opening the decoder
    AVFrame                 *decoded_params;

    AVBSFContext            *bsf;

    /* number of packets successfully read for this stream */
    uint64_t                 nb_packets;
    // combined size of all the packets read
    uint64_t                 data_size;
    // latest wallclock time at which packet reading resumed after a stall - used for readrate
    int64_t                  resume_wc;
    // timestamp of first packet sent after the latest stall - used for readrate
    int64_t                  resume_pts;
    // measure of how far behind packet reading is against spceified readrate
    int64_t                  lag;
} DemuxStream;

typedef struct DemuxStreamGroup {
    InputStreamGroup         istg;

    // name used for logging
    char                     log_name[32];
} DemuxStreamGroup;

typedef struct Demuxer {
    InputFile             f;

    // name used for logging
    char                  log_name[32];

    int64_t               wallclock_start;

    /**
     * Extra timestamp offset added by discontinuity handling.
     */
    int64_t               ts_offset_discont;
    int64_t               last_ts;

    int64_t               recording_time;
    int                   accurate_seek;

    /* number of times input stream should be looped */
    int                   loop;
    int                   have_audio_dec;
    /* duration of the looped segment of the input file */
    Timestamp             duration;
    /* pts with the smallest/largest values ever seen */
    Timestamp             min_pts;
    Timestamp             max_pts;

    /* number of streams that the user was warned of */
    int                   nb_streams_warn;

    float                 readrate;
    double                readrate_initial_burst;
    float                 readrate_catchup;

    Scheduler            *sch;

    AVPacket             *pkt_heartbeat;

    int                   read_started;
    int                   nb_streams_used;
    int                   nb_streams_finished;
} Demuxer;

typedef struct DemuxThreadContext {
    // packet used for reading from the demuxer
    AVPacket *pkt_demux;
    // packet for reading from BSFs
    AVPacket *pkt_bsf;
} DemuxThreadContext;

static DemuxStream *ds_from_ist(InputStream *ist)
{
    return (DemuxStream*)ist;
}

static Demuxer *demuxer_from_ifile(InputFile *f)
{
    return (Demuxer*)f;
}

InputStream *ist_find_unused(enum AVMediaType type)
{
    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        DemuxStream *ds = ds_from_ist(ist);
        if (ist->par->codec_type == type && ds->discard &&
            ist->user_set_discard != AVDISCARD_ALL)
            return ist;
    }
    return NULL;
}

static void report_new_stream(Demuxer *d, const AVPacket *pkt)
{
    const AVStream *st = d->f.ctx->streams[pkt->stream_index];

    if (pkt->stream_index < d->nb_streams_warn)
        return;
    av_log(d, AV_LOG_WARNING,
           "New %s stream with index %d at pos:%"PRId64" and DTS:%ss\n",
           av_get_media_type_string(st->codecpar->codec_type),
           pkt->stream_index, pkt->pos, av_ts2timestr(pkt->dts, &st->time_base));
    d->nb_streams_warn = pkt->stream_index + 1;
}

static int seek_to_start(Demuxer *d, Timestamp end_pts)
{
    InputFile    *ifile = &d->f;
    AVFormatContext *is = ifile->ctx;
    int ret;

    ret = avformat_seek_file(is, -1, INT64_MIN, is->start_time, is->start_time, 0);
    if (ret < 0)
        return ret;

    if (end_pts.ts != AV_NOPTS_VALUE &&
        (d->max_pts.ts == AV_NOPTS_VALUE ||
         av_compare_ts(d->max_pts.ts, d->max_pts.tb, end_pts.ts, end_pts.tb) < 0))
        d->max_pts = end_pts;

    if (d->max_pts.ts != AV_NOPTS_VALUE) {
        int64_t min_pts = d->min_pts.ts == AV_NOPTS_VALUE ? 0 : d->min_pts.ts;
        d->duration.ts = d->max_pts.ts - av_rescale_q(min_pts, d->min_pts.tb, d->max_pts.tb);
    }
    d->duration.tb = d->max_pts.tb;

    if (d->loop > 0)
        d->loop--;

    return ret;
}

static void ts_discontinuity_detect(Demuxer *d, InputStream *ist,
                                    AVPacket *pkt)
{
    InputFile *ifile = &d->f;
    DemuxStream *ds = ds_from_ist(ist);
    const int fmt_is_discont = ifile->ctx->iformat->flags & AVFMT_TS_DISCONT;
    int disable_discontinuity_correction = copy_ts;
    int64_t pkt_dts = av_rescale_q_rnd(pkt->dts, pkt->time_base, AV_TIME_BASE_Q,
                                       AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

    if (copy_ts && ds->next_dts != AV_NOPTS_VALUE &&
        fmt_is_discont && ist->st->pts_wrap_bits < 60) {
        int64_t wrap_dts = av_rescale_q_rnd(pkt->dts + (1LL<<ist->st->pts_wrap_bits),
                                            pkt->time_base, AV_TIME_BASE_Q,
                                            AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        if (FFABS(wrap_dts - ds->next_dts) < FFABS(pkt_dts - ds->next_dts)/10)
            disable_discontinuity_correction = 0;
    }

    if (ds->next_dts != AV_NOPTS_VALUE && !disable_discontinuity_correction) {
        int64_t delta = pkt_dts - ds->next_dts;
        if (fmt_is_discont) {
            if (FFABS(delta) > 1LL * dts_delta_threshold * AV_TIME_BASE ||
                pkt_dts + AV_TIME_BASE/10 < ds->dts) {
                d->ts_offset_discont -= delta;
                av_log(ist, AV_LOG_WARNING,
                       "timestamp discontinuity "
                       "(stream id=%d): %"PRId64", new offset= %"PRId64"\n",
                       ist->st->id, delta, d->ts_offset_discont);
                pkt->dts -= av_rescale_q(delta, AV_TIME_BASE_Q, pkt->time_base);
                if (pkt->pts != AV_NOPTS_VALUE)
                    pkt->pts -= av_rescale_q(delta, AV_TIME_BASE_Q, pkt->time_base);
            }
        } else {
            if (FFABS(delta) > 1LL * dts_error_threshold * AV_TIME_BASE) {
                av_log(ist, AV_LOG_WARNING,
                       "DTS %"PRId64", next:%"PRId64" st:%d invalid dropping\n",
                       pkt->dts, ds->next_dts, pkt->stream_index);
                pkt->dts = AV_NOPTS_VALUE;
            }
            if (pkt->pts != AV_NOPTS_VALUE){
                int64_t pkt_pts = av_rescale_q(pkt->pts, pkt->time_base, AV_TIME_BASE_Q);
                delta = pkt_pts - ds->next_dts;
                if (FFABS(delta) > 1LL * dts_error_threshold * AV_TIME_BASE) {
                    av_log(ist, AV_LOG_WARNING,
                           "PTS %"PRId64", next:%"PRId64" invalid dropping st:%d\n",
                           pkt->pts, ds->next_dts, pkt->stream_index);
                    pkt->pts = AV_NOPTS_VALUE;
                }
            }
        }
    } else if (ds->next_dts == AV_NOPTS_VALUE && !copy_ts &&
               fmt_is_discont && d->last_ts != AV_NOPTS_VALUE) {
        int64_t delta = pkt_dts - d->last_ts;
        if (FFABS(delta) > 1LL * dts_delta_threshold * AV_TIME_BASE) {
            d->ts_offset_discont -= delta;
            av_log(ist, AV_LOG_DEBUG,
                   "Inter stream timestamp discontinuity %"PRId64", new offset= %"PRId64"\n",
                   delta, d->ts_offset_discont);
            pkt->dts -= av_rescale_q(delta, AV_TIME_BASE_Q, pkt->time_base);
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts -= av_rescale_q(delta, AV_TIME_BASE_Q, pkt->time_base);
        }
    }

    d->last_ts = av_rescale_q(pkt->dts, pkt->time_base, AV_TIME_BASE_Q);
}

static void ts_discontinuity_process(Demuxer *d, InputStream *ist,
                                     AVPacket *pkt)
{
    int64_t offset = av_rescale_q(d->ts_offset_discont, AV_TIME_BASE_Q,
                                  pkt->time_base);

    // apply previously-detected timestamp-discontinuity offset
    // (to all streams, not just audio/video)
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts += offset;
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts += offset;

    // detect timestamp discontinuities for audio/video
    if ((ist->par->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->par->codec_type == AVMEDIA_TYPE_AUDIO) &&
        pkt->dts != AV_NOPTS_VALUE)
        ts_discontinuity_detect(d, ist, pkt);
}

static int ist_dts_update(DemuxStream *ds, AVPacket *pkt, FrameData *fd)
{
    InputStream *ist = &ds->ist;
    const AVCodecParameters *par = ist->par;

    if (!ds->saw_first_ts) {
        ds->first_dts =
        ds->dts = ist->st->avg_frame_rate.num ? - ist->par->video_delay * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
        if (pkt->pts != AV_NOPTS_VALUE) {
            ds->first_dts =
            ds->dts += av_rescale_q(pkt->pts, pkt->time_base, AV_TIME_BASE_Q);
        }
        ds->saw_first_ts = 1;
    }

    if (ds->next_dts == AV_NOPTS_VALUE)
        ds->next_dts = ds->dts;

    if (pkt->dts != AV_NOPTS_VALUE)
        ds->next_dts = ds->dts = av_rescale_q(pkt->dts, pkt->time_base, AV_TIME_BASE_Q);

    ds->dts = ds->next_dts;
    switch (par->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        av_assert1(pkt->duration >= 0);
        if (par->sample_rate) {
            ds->next_dts += ((int64_t)AV_TIME_BASE * par->frame_size) /
                              par->sample_rate;
        } else {
            ds->next_dts += av_rescale_q(pkt->duration, pkt->time_base, AV_TIME_BASE_Q);
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (ist->framerate.num) {
            // TODO: Remove work-around for c99-to-c89 issue 7
            AVRational time_base_q = AV_TIME_BASE_Q;
            int64_t next_dts = av_rescale_q(ds->next_dts, time_base_q, av_inv_q(ist->framerate));
            ds->next_dts = av_rescale_q(next_dts + 1, av_inv_q(ist->framerate), time_base_q);
        } else if (pkt->duration) {
            ds->next_dts += av_rescale_q(pkt->duration, pkt->time_base, AV_TIME_BASE_Q);
        } else if (ist->par->framerate.num != 0) {
            AVRational field_rate = av_mul_q(ist->par->framerate,
                                             (AVRational){ 2, 1 });
            int fields = 2;

            if (ds->codec_desc                                 &&
                (ds->codec_desc->props & AV_CODEC_PROP_FIELDS) &&
                av_stream_get_parser(ist->st))
                fields = 1 + av_stream_get_parser(ist->st)->repeat_pict;

            ds->next_dts += av_rescale_q(fields, av_inv_q(field_rate), AV_TIME_BASE_Q);
        }
        break;
    }

    fd->dts_est = ds->dts;

    return 0;
}

static int ts_fixup(Demuxer *d, AVPacket *pkt, FrameData *fd)
{
    InputFile *ifile = &d->f;
    InputStream *ist = ifile->streams[pkt->stream_index];
    DemuxStream  *ds = ds_from_ist(ist);
    const int64_t start_time = ifile->start_time_effective;
    int64_t duration;
    int ret;

    pkt->time_base = ist->st->time_base;

#define SHOW_TS_DEBUG(tag_)                                             \
    if (debug_ts) {                                                     \
        av_log(ist, AV_LOG_INFO, "%s -> ist_index:%d:%d type:%s "       \
               "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s duration:%s duration_time:%s\n", \
               tag_, ifile->index, pkt->stream_index,                   \
               av_get_media_type_string(ist->st->codecpar->codec_type), \
               av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &pkt->time_base), \
               av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &pkt->time_base), \
               av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &pkt->time_base)); \
    }

    SHOW_TS_DEBUG("demuxer");

    if (!ds->wrap_correction_done && start_time != AV_NOPTS_VALUE &&
        ist->st->pts_wrap_bits < 64) {
        int64_t stime, stime2;

        stime = av_rescale_q(start_time, AV_TIME_BASE_Q, pkt->time_base);
        stime2= stime + (1ULL<<ist->st->pts_wrap_bits);
        ds->wrap_correction_done = 1;

        if(stime2 > stime && pkt->dts != AV_NOPTS_VALUE && pkt->dts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt->dts -= 1ULL<<ist->st->pts_wrap_bits;
            ds->wrap_correction_done = 0;
        }
        if(stime2 > stime && pkt->pts != AV_NOPTS_VALUE && pkt->pts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt->pts -= 1ULL<<ist->st->pts_wrap_bits;
            ds->wrap_correction_done = 0;
        }
    }

    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, pkt->time_base);
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, pkt->time_base);

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts *= ds->ts_scale;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts *= ds->ts_scale;

    duration = av_rescale_q(d->duration.ts, d->duration.tb, pkt->time_base);
    if (pkt->pts != AV_NOPTS_VALUE) {
        // audio decoders take precedence for estimating total file duration
        int64_t pkt_duration = d->have_audio_dec ? 0 : pkt->duration;

        pkt->pts += duration;

        // update max/min pts that will be used to compute total file duration
        // when using -stream_loop
        if (d->max_pts.ts == AV_NOPTS_VALUE ||
            av_compare_ts(d->max_pts.ts, d->max_pts.tb,
                          pkt->pts + pkt_duration, pkt->time_base) < 0) {
            d->max_pts = (Timestamp){ .ts = pkt->pts + pkt_duration,
                                      .tb = pkt->time_base };
        }
        if (d->min_pts.ts == AV_NOPTS_VALUE ||
            av_compare_ts(d->min_pts.ts, d->min_pts.tb,
                          pkt->pts, pkt->time_base) > 0) {
            d->min_pts = (Timestamp){ .ts = pkt->pts,
                                      .tb = pkt->time_base };
        }
    }

    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts += duration;

    SHOW_TS_DEBUG("demuxer+tsfixup");

    // detect and try to correct for timestamp discontinuities
    ts_discontinuity_process(d, ist, pkt);

    // update estimated/predicted dts
    ret = ist_dts_update(ds, pkt, fd);
    if (ret < 0)
        return ret;

    return 0;
}

static int input_packet_process(Demuxer *d, AVPacket *pkt, unsigned *send_flags)
{
    InputFile     *f = &d->f;
    InputStream *ist = f->streams[pkt->stream_index];
    DemuxStream  *ds = ds_from_ist(ist);
    FrameData *fd;
    int ret = 0;

    fd = packet_data(pkt);
    if (!fd)
        return AVERROR(ENOMEM);

    ret = ts_fixup(d, pkt, fd);
    if (ret < 0)
        return ret;

    if (d->recording_time != INT64_MAX) {
        int64_t start_time = 0;
        if (copy_ts) {
            start_time += f->start_time != AV_NOPTS_VALUE ? f->start_time : 0;
            start_time += start_at_zero ? 0 : f->start_time_effective;
        }
        if (ds->dts >= d->recording_time + start_time)
            *send_flags |= DEMUX_SEND_STREAMCOPY_EOF;
    }

    ds->data_size += pkt->size;
    ds->nb_packets++;

    fd->wallclock[LATENCY_PROBE_DEMUX] = av_gettime_relative();

    if (debug_ts) {
        av_log(ist, AV_LOG_INFO, "demuxer+ffmpeg -> ist_index:%d:%d type:%s pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s duration:%s duration_time:%s off:%s off_time:%s\n",
               f->index, pkt->stream_index,
               av_get_media_type_string(ist->par->codec_type),
               av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &pkt->time_base),
               av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &pkt->time_base),
               av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &pkt->time_base),
               av_ts2str(f->ts_offset),  av_ts2timestr(f->ts_offset, &AV_TIME_BASE_Q));
    }

    return 0;
}

static void readrate_sleep(Demuxer *d)
{
    InputFile *f = &d->f;
    int64_t file_start = copy_ts * (
                          (f->start_time_effective != AV_NOPTS_VALUE ? f->start_time_effective * !start_at_zero : 0) +
                          (f->start_time != AV_NOPTS_VALUE ? f->start_time : 0)
                         );
    int64_t initial_burst = AV_TIME_BASE * d->readrate_initial_burst;
    int resume_warn = 0;

    for (int i = 0; i < f->nb_streams; i++) {
        InputStream *ist = f->streams[i];
        DemuxStream  *ds = ds_from_ist(ist);
        int64_t stream_ts_offset, pts, now, wc_elapsed, elapsed, lag, max_pts, limit_pts;

        if (ds->discard) continue;

        stream_ts_offset = FFMAX(ds->first_dts != AV_NOPTS_VALUE ? ds->first_dts : 0, file_start);
        pts = av_rescale(ds->dts, 1000000, AV_TIME_BASE);
        now = av_gettime_relative();
        wc_elapsed = now - d->wallclock_start;

        if (pts <= stream_ts_offset + initial_burst) continue;

        max_pts = stream_ts_offset + initial_burst + (int64_t)(wc_elapsed * d->readrate);
        lag = FFMAX(max_pts - pts, 0);
        if ( (!ds->lag && lag > 0.3 * AV_TIME_BASE) || ( lag > ds->lag + 0.3 * AV_TIME_BASE) ) {
            ds->lag = lag;
            ds->resume_wc = now;
            ds->resume_pts = pts;
            av_log_once(ds, AV_LOG_WARNING, AV_LOG_DEBUG, &resume_warn,
                        "Resumed reading at pts %0.3f with rate %0.3f after a lag of %0.3fs\n",
                        (float)pts/AV_TIME_BASE, d->readrate_catchup, (float)lag/AV_TIME_BASE);
        }
        if (ds->lag && !lag)
            ds->lag = ds->resume_wc = ds->resume_pts = 0;
        if (ds->resume_wc) {
            elapsed = now - ds->resume_wc;
            limit_pts = ds->resume_pts + (int64_t)(elapsed * d->readrate_catchup);
        } else {
            elapsed = wc_elapsed;
            limit_pts = max_pts;
        }

        if (pts > limit_pts)
            av_usleep(pts - limit_pts);
    }
}

static int do_send(Demuxer *d, DemuxStream *ds, AVPacket *pkt, unsigned flags,
                   const char *pkt_desc)
{
    int ret;

    pkt->stream_index = ds->sch_idx_stream;

    ret = sch_demux_send(d->sch, d->f.index, pkt, flags);
    if (ret == AVERROR_EOF) {
        av_packet_unref(pkt);

        av_log(ds, AV_LOG_VERBOSE, "All consumers of this stream are done\n");
        ds->finished = 1;

        if (++d->nb_streams_finished == d->nb_streams_used) {
            av_log(d, AV_LOG_VERBOSE, "All consumers are done\n");
            return AVERROR_EOF;
        }
    } else if (ret < 0) {
        if (ret != AVERROR_EXIT)
            av_log(d, AV_LOG_ERROR,
                   "Unable to send %s packet to consumers: %s\n",
                   pkt_desc, av_err2str(ret));
        return ret;
    }

    return 0;
}

static int demux_send(Demuxer *d, DemuxThreadContext *dt, DemuxStream *ds,
                      AVPacket *pkt, unsigned flags)
{
    InputFile  *f = &d->f;
    int ret;

    // pkt can be NULL only when flushing BSFs
    av_assert0(ds->bsf || pkt);

    // send heartbeat for sub2video streams
    if (d->pkt_heartbeat && pkt && pkt->pts != AV_NOPTS_VALUE) {
        for (int i = 0; i < f->nb_streams; i++) {
            DemuxStream *ds1 = ds_from_ist(f->streams[i]);

            if (ds1->finished || !ds1->have_sub2video)
                continue;

            d->pkt_heartbeat->pts          = pkt->pts;
            d->pkt_heartbeat->time_base    = pkt->time_base;
            d->pkt_heartbeat->opaque       = (void*)(intptr_t)PKT_OPAQUE_SUB_HEARTBEAT;

            ret = do_send(d, ds1, d->pkt_heartbeat, 0, "heartbeat");
            if (ret < 0)
                return ret;
        }
    }

    if (ds->bsf) {
        if (pkt)
            av_packet_rescale_ts(pkt, pkt->time_base, ds->bsf->time_base_in);

        ret = av_bsf_send_packet(ds->bsf, pkt);
        if (ret < 0) {
            if (pkt)
                av_packet_unref(pkt);
            av_log(ds, AV_LOG_ERROR, "Error submitting a packet for filtering: %s\n",
                   av_err2str(ret));
            return ret;
        }

        while (1) {
            ret = av_bsf_receive_packet(ds->bsf, dt->pkt_bsf);
            if (ret == AVERROR(EAGAIN))
                return 0;
            else if (ret < 0) {
                if (ret != AVERROR_EOF)
                    av_log(ds, AV_LOG_ERROR,
                           "Error applying bitstream filters to a packet: %s\n",
                           av_err2str(ret));
                return ret;
            }

            dt->pkt_bsf->time_base = ds->bsf->time_base_out;

            ret = do_send(d, ds, dt->pkt_bsf, 0, "filtered");
            if (ret < 0) {
                av_packet_unref(dt->pkt_bsf);
                return ret;
            }
        }
    } else {
        ret = do_send(d, ds, pkt, flags, "demuxed");
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int demux_bsf_flush(Demuxer *d, DemuxThreadContext *dt)
{
    InputFile *f = &d->f;
    int ret;

    for (unsigned i = 0; i < f->nb_streams; i++) {
        DemuxStream *ds = ds_from_ist(f->streams[i]);

        if (!ds->bsf)
            continue;

        ret = demux_send(d, dt, ds, NULL, 0);
        ret = (ret == AVERROR_EOF) ? 0 : (ret < 0) ? ret : AVERROR_BUG;
        if (ret < 0) {
            av_log(ds, AV_LOG_ERROR, "Error flushing BSFs: %s\n",
                   av_err2str(ret));
            return ret;
        }

        av_bsf_flush(ds->bsf);
    }

    return 0;
}

static void discard_unused_programs(InputFile *ifile)
{
    for (int j = 0; j < ifile->ctx->nb_programs; j++) {
        AVProgram *p = ifile->ctx->programs[j];
        int discard  = AVDISCARD_ALL;

        for (int k = 0; k < p->nb_stream_indexes; k++) {
            DemuxStream *ds = ds_from_ist(ifile->streams[p->stream_index[k]]);

            if (!ds->discard) {
                discard = AVDISCARD_DEFAULT;
                break;
            }
        }
        p->discard = discard;
    }
}

static void demux_thread_set_name(InputFile *f)
{
    char name[16];
    snprintf(name, sizeof(name), "dmx%d:%s", f->index, f->ctx->iformat->name);
    ff_thread_setname(name);
}

static void demux_thread_uninit(DemuxThreadContext *dt)
{
    av_packet_free(&dt->pkt_demux);
    av_packet_free(&dt->pkt_bsf);

    memset(dt, 0, sizeof(*dt));
}

static int demux_thread_init(DemuxThreadContext *dt)
{
    memset(dt, 0, sizeof(*dt));

    dt->pkt_demux = av_packet_alloc();
    if (!dt->pkt_demux)
        return AVERROR(ENOMEM);

    dt->pkt_bsf = av_packet_alloc();
    if (!dt->pkt_bsf)
        return AVERROR(ENOMEM);

    return 0;
}

static int input_thread(void *arg)
{
    Demuxer   *d = arg;
    InputFile *f = &d->f;

    DemuxThreadContext dt;

    int ret = 0;

    ret = demux_thread_init(&dt);
    if (ret < 0)
        goto finish;

    demux_thread_set_name(f);

    discard_unused_programs(f);

    d->read_started    = 1;
    d->wallclock_start = av_gettime_relative();

    while (1) {
        DemuxStream *ds;
        unsigned send_flags = 0;

        ret = av_read_frame(f->ctx, dt.pkt_demux);

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (ret < 0) {
            int ret_bsf;

            if (ret == AVERROR_EOF)
                av_log(d, AV_LOG_VERBOSE, "EOF while reading input\n");
            else {
                av_log(d, AV_LOG_ERROR, "Error during demuxing: %s\n",
                       av_err2str(ret));
                ret = exit_on_error ? ret : 0;
            }

            ret_bsf = demux_bsf_flush(d, &dt);
            ret = err_merge(ret == AVERROR_EOF ? 0 : ret, ret_bsf);

            if (d->loop) {
                /* signal looping to our consumers */
                dt.pkt_demux->stream_index = -1;
                ret = sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
                if (ret >= 0)
                    ret = seek_to_start(d, (Timestamp){ .ts = dt.pkt_demux->pts,
                                                        .tb = dt.pkt_demux->time_base });
                if (ret >= 0)
                    continue;

                /* fallthrough to the error path */
            }

            break;
        }

        if (do_pkt_dump) {
            av_pkt_dump_log2(NULL, AV_LOG_INFO, dt.pkt_demux, do_hex_dump,
                             f->ctx->streams[dt.pkt_demux->stream_index]);
        }

        /* the following test is needed in case new streams appear
           dynamically in stream : we ignore them */
        ds = dt.pkt_demux->stream_index < f->nb_streams ?
             ds_from_ist(f->streams[dt.pkt_demux->stream_index]) : NULL;
        if (!ds || ds->discard || ds->finished) {
            report_new_stream(d, dt.pkt_demux);
            av_packet_unref(dt.pkt_demux);
            continue;
        }

        if (dt.pkt_demux->flags & AV_PKT_FLAG_CORRUPT) {
            av_log(d, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "corrupt input packet in stream %d\n",
                   dt.pkt_demux->stream_index);
            if (exit_on_error) {
                av_packet_unref(dt.pkt_demux);
                ret = AVERROR_INVALIDDATA;
                break;
            }
        }

        ret = input_packet_process(d, dt.pkt_demux, &send_flags);
        if (ret < 0)
            break;

        if (d->readrate)
            readrate_sleep(d);

        ret = demux_send(d, &dt, ds, dt.pkt_demux, send_flags);
        if (ret < 0)
            break;
    }

    // EOF/EXIT is normal termination
    if (ret == AVERROR_EOF || ret == AVERROR_EXIT)
        ret = 0;

finish:
    demux_thread_uninit(&dt);

    return ret;
}

static void demux_final_stats(Demuxer *d)
{
    InputFile *f = &d->f;
    uint64_t total_packets = 0, total_size = 0;

    av_log(f, AV_LOG_VERBOSE, "Input file #%d (%s):\n",
           f->index, f->ctx->url);

    for (int j = 0; j < f->nb_streams; j++) {
        InputStream *ist = f->streams[j];
        DemuxStream  *ds = ds_from_ist(ist);
        enum AVMediaType type = ist->par->codec_type;

        if (ds->discard || type == AVMEDIA_TYPE_ATTACHMENT)
            continue;

        total_size    += ds->data_size;
        total_packets += ds->nb_packets;

        av_log(f, AV_LOG_VERBOSE, "  Input stream #%d:%d (%s): ",
               f->index, j, av_get_media_type_string(type));
        av_log(f, AV_LOG_VERBOSE, "%"PRIu64" packets read (%"PRIu64" bytes); ",
               ds->nb_packets, ds->data_size);

        if (ds->decoding_needed) {
            av_log(f, AV_LOG_VERBOSE,
                   "%"PRIu64" frames decoded; %"PRIu64" decode errors",
                   ist->decoder->frames_decoded, ist->decoder->decode_errors);
            if (type == AVMEDIA_TYPE_AUDIO)
                av_log(f, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ist->decoder->samples_decoded);
            av_log(f, AV_LOG_VERBOSE, "; ");
        }

        av_log(f, AV_LOG_VERBOSE, "\n");
    }

    av_log(f, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) demuxed\n",
           total_packets, total_size);
}

static void ist_free(InputStream **pist)
{
    InputStream *ist = *pist;
    DemuxStream *ds;

    if (!ist)
        return;
    ds = ds_from_ist(ist);

    dec_free(&ist->decoder);

    av_dict_free(&ds->decoder_opts);
    av_freep(&ist->filters);
    av_freep(&ds->dec_opts.hwaccel_device);

    avcodec_parameters_free(&ist->par);

    av_frame_free(&ds->decoded_params);

    av_bsf_free(&ds->bsf);

    av_freep(pist);
}

static void istg_free(InputStreamGroup **pistg)
{
    InputStreamGroup *istg = *pistg;

    if (!istg)
        return;

    av_freep(pistg);
}

void ifile_close(InputFile **pf)
{
    InputFile *f = *pf;
    Demuxer   *d = demuxer_from_ifile(f);

    if (!f)
        return;

    if (d->read_started)
        demux_final_stats(d);

    for (int i = 0; i < f->nb_streams; i++)
        ist_free(&f->streams[i]);
    av_freep(&f->streams);

    for (int i = 0; i < f->nb_stream_groups; i++)
        istg_free(&f->stream_groups[i]);
    av_freep(&f->stream_groups);

    avformat_close_input(&f->ctx);

    av_packet_free(&d->pkt_heartbeat);

    av_freep(pf);
}

int ist_use(InputStream *ist, int decoding_needed,
            const ViewSpecifier *vs, SchedulerNode *src)
{
    Demuxer      *d = demuxer_from_ifile(ist->file);
    DemuxStream *ds = ds_from_ist(ist);
    int ret;

    if (ist->user_set_discard == AVDISCARD_ALL) {
        av_log(ist, AV_LOG_ERROR, "Cannot %s a disabled input stream\n",
               decoding_needed ? "decode" : "streamcopy");
        return AVERROR(EINVAL);
    }

    if (decoding_needed && !ist->dec) {
        av_log(ist, AV_LOG_ERROR,
               "Decoding requested, but no decoder found for: %s\n",
                avcodec_get_name(ist->par->codec_id));
        return AVERROR(EINVAL);
    }

    if (ds->sch_idx_stream < 0) {
        ret = sch_add_demux_stream(d->sch, d->f.index);
        if (ret < 0)
            return ret;
        ds->sch_idx_stream = ret;
    }

    if (ds->discard) {
        ds->discard = 0;
        d->nb_streams_used++;
    }

    ist->st->discard      = ist->user_set_discard;
    ds->decoding_needed   |= decoding_needed;
    ds->streamcopy_needed |= !decoding_needed;

    if (decoding_needed && ds->sch_idx_dec < 0) {
        int is_audio = ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        int is_unreliable = !!(d->f.ctx->iformat->flags & AVFMT_NOTIMESTAMPS);
        int64_t use_wallclock_as_timestamps;

        ret = av_opt_get_int(d->f.ctx, "use_wallclock_as_timestamps", 0, &use_wallclock_as_timestamps);
        if (ret < 0)
            return ret;

        if (use_wallclock_as_timestamps)
            is_unreliable = 0;

        ds->dec_opts.flags |= (!!ist->fix_sub_duration * DECODER_FLAG_FIX_SUB_DURATION) |
                              (!!is_unreliable * DECODER_FLAG_TS_UNRELIABLE) |
                              (!!(d->loop && is_audio) * DECODER_FLAG_SEND_END_TS)
#if FFMPEG_OPT_TOP
                              | ((ist->top_field_first >= 0) * DECODER_FLAG_TOP_FIELD_FIRST)
#endif
                             ;

        if (ist->framerate.num) {
            ds->dec_opts.flags     |= DECODER_FLAG_FRAMERATE_FORCED;
            ds->dec_opts.framerate  = ist->framerate;
        } else
            ds->dec_opts.framerate  = ist->st->avg_frame_rate;

        if (ist->dec->id == AV_CODEC_ID_DVB_SUBTITLE &&
           (ds->decoding_needed & DECODING_FOR_OST)) {
            av_dict_set(&ds->decoder_opts, "compute_edt", "1", AV_DICT_DONT_OVERWRITE);
            if (ds->decoding_needed & DECODING_FOR_FILTER)
                av_log(ist, AV_LOG_WARNING,
                       "Warning using DVB subtitles for filtering and output at the "
                       "same time is not fully supported, also see -compute_edt [0|1]\n");
        }

        snprintf(ds->dec_name, sizeof(ds->dec_name), "%d:%d", ist->file->index, ist->index);
        ds->dec_opts.name = ds->dec_name;

        ds->dec_opts.codec = ist->dec;
        ds->dec_opts.par   = ist->par;

        ds->dec_opts.log_parent = ist;

        ds->decoded_params = av_frame_alloc();
        if (!ds->decoded_params)
            return AVERROR(ENOMEM);

        ret = dec_init(&ist->decoder, d->sch,
                       &ds->decoder_opts, &ds->dec_opts, ds->decoded_params);
        if (ret < 0)
            return ret;
        ds->sch_idx_dec = ret;

        ret = sch_connect(d->sch, SCH_DSTREAM(d->f.index, ds->sch_idx_stream),
                                  SCH_DEC_IN(ds->sch_idx_dec));
        if (ret < 0)
            return ret;

        d->have_audio_dec |= is_audio;
    }

    if (decoding_needed && ist->par->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = dec_request_view(ist->decoder, vs, src);
        if (ret < 0)
            return ret;
    } else {
        *src = decoding_needed                             ?
               SCH_DEC_OUT(ds->sch_idx_dec, 0)             :
               SCH_DSTREAM(d->f.index, ds->sch_idx_stream);
    }

    return 0;
}

int ist_filter_add(InputStream *ist, InputFilter *ifilter, int is_simple,
                   const ViewSpecifier *vs, InputFilterOptions *opts,
                   SchedulerNode *src)
{
    Demuxer      *d = demuxer_from_ifile(ist->file);
    DemuxStream *ds = ds_from_ist(ist);
    int64_t tsoffset = 0;
    int ret;

    ret = ist_use(ist, is_simple ? DECODING_FOR_OST : DECODING_FOR_FILTER,
                  vs, src);
    if (ret < 0)
        return ret;

    ret = GROW_ARRAY(ist->filters, ist->nb_filters);
    if (ret < 0)
        return ret;

    ist->filters[ist->nb_filters - 1] = ifilter;

    if (ist->par->codec_type == AVMEDIA_TYPE_VIDEO) {
        const AVPacketSideData *sd = av_packet_side_data_get(ist->par->coded_side_data,
                                                             ist->par->nb_coded_side_data,
                                                             AV_PKT_DATA_FRAME_CROPPING);
        if (ist->framerate.num > 0 && ist->framerate.den > 0) {
            opts->framerate = ist->framerate;
            opts->flags |= IFILTER_FLAG_CFR;
        } else
            opts->framerate = av_guess_frame_rate(d->f.ctx, ist->st, NULL);
        if (sd && sd->size >= sizeof(uint32_t) * 4) {
            opts->crop_top    = AV_RL32(sd->data +  0);
            opts->crop_bottom = AV_RL32(sd->data +  4);
            opts->crop_left   = AV_RL32(sd->data +  8);
            opts->crop_right  = AV_RL32(sd->data + 12);
            if (ds->apply_cropping && ds->apply_cropping != CROP_CODEC &&
                (opts->crop_top | opts->crop_bottom | opts->crop_left | opts->crop_right))
                opts->flags |= IFILTER_FLAG_CROP;
        }
    } else if (ist->par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        /* Compute the size of the canvas for the subtitles stream.
           If the subtitles codecpar has set a size, use it. Otherwise use the
           maximum dimensions of the video streams in the same file. */
        opts->sub2video_width  = ist->par->width;
        opts->sub2video_height = ist->par->height;
        if (!(opts->sub2video_width && opts->sub2video_height)) {
            for (int j = 0; j < d->f.nb_streams; j++) {
                AVCodecParameters *par1 = d->f.streams[j]->par;
                if (par1->codec_type == AVMEDIA_TYPE_VIDEO) {
                    opts->sub2video_width  = FFMAX(opts->sub2video_width,  par1->width);
                    opts->sub2video_height = FFMAX(opts->sub2video_height, par1->height);
                }
            }
        }

        if (!(opts->sub2video_width && opts->sub2video_height)) {
            opts->sub2video_width  = FFMAX(opts->sub2video_width,  720);
            opts->sub2video_height = FFMAX(opts->sub2video_height, 576);
        }

        if (!d->pkt_heartbeat) {
            d->pkt_heartbeat = av_packet_alloc();
            if (!d->pkt_heartbeat)
                return AVERROR(ENOMEM);
        }
        ds->have_sub2video = 1;
    }

    ret = av_frame_copy_props(opts->fallback, ds->decoded_params);
    if (ret < 0)
        return ret;
    opts->fallback->format = ds->decoded_params->format;
    opts->fallback->width  = ds->decoded_params->width;
    opts->fallback->height = ds->decoded_params->height;

    ret = av_channel_layout_copy(&opts->fallback->ch_layout, &ds->decoded_params->ch_layout);
    if (ret < 0)
        return ret;

    if (copy_ts) {
        tsoffset = d->f.start_time == AV_NOPTS_VALUE ? 0 : d->f.start_time;
        if (!start_at_zero && d->f.ctx->start_time != AV_NOPTS_VALUE)
            tsoffset += d->f.ctx->start_time;
    }
    opts->trim_start_us = ((d->f.start_time == AV_NOPTS_VALUE) || !d->accurate_seek) ?
                          AV_NOPTS_VALUE : tsoffset;
    opts->trim_end_us   = d->recording_time;

    opts->name = av_strdup(ds->dec_name);
    if (!opts->name)
        return AVERROR(ENOMEM);

    opts->flags |= IFILTER_FLAG_AUTOROTATE * !!(ds->autorotate) |
                   IFILTER_FLAG_REINIT     * !!(ds->reinit_filters) |
                   IFILTER_FLAG_DROPCHANGED* !!(ds->drop_changed);

    return 0;
}

static int choose_decoder(const OptionsContext *o, void *logctx,
                          AVFormatContext *s, AVStream *st,
                          enum HWAccelID hwaccel_id, enum AVHWDeviceType hwaccel_device_type,
                          const AVCodec **pcodec)

{
    const char *codec_name = NULL;

    opt_match_per_stream_str(logctx, &o->codec_names, s, st, &codec_name);
    if (codec_name) {
        int ret = find_codec(NULL, codec_name, st->codecpar->codec_type, 0, pcodec);
        if (ret < 0)
            return ret;
        st->codecpar->codec_id = (*pcodec)->id;
        if (recast_media && st->codecpar->codec_type != (*pcodec)->type)
            st->codecpar->codec_type = (*pcodec)->type;
        return 0;
    } else {
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            hwaccel_id == HWACCEL_GENERIC &&
            hwaccel_device_type != AV_HWDEVICE_TYPE_NONE) {
            const AVCodec *c;
            void *i = NULL;

            while ((c = av_codec_iterate(&i))) {
                const AVCodecHWConfig *config;

                if (c->id != st->codecpar->codec_id ||
                    !av_codec_is_decoder(c))
                    continue;

                for (int j = 0; config = avcodec_get_hw_config(c, j); j++) {
                    if (config->device_type == hwaccel_device_type) {
                        av_log(logctx, AV_LOG_VERBOSE, "Selecting decoder '%s' because of requested hwaccel method %s\n",
                               c->name, av_hwdevice_get_type_name(hwaccel_device_type));
                        *pcodec = c;
                        return 0;
                    }
                }
            }
        }

        *pcodec = avcodec_find_decoder(st->codecpar->codec_id);
        return 0;
    }
}

static int guess_input_channel_layout(InputStream *ist, AVCodecParameters *par,
                                      int guess_layout_max)
{
    if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        char layout_name[256];

        if (par->ch_layout.nb_channels > guess_layout_max)
            return 0;
        av_channel_layout_default(&par->ch_layout, par->ch_layout.nb_channels);
        if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            return 0;
        av_channel_layout_describe(&par->ch_layout, layout_name, sizeof(layout_name));
        av_log(ist, AV_LOG_WARNING, "Guessed Channel Layout: %s\n", layout_name);
    }
    return 1;
}

static int add_display_matrix_to_stream(const OptionsContext *o,
                                        AVFormatContext *ctx, InputStream *ist)
{
    AVStream *st = ist->st;
    DemuxStream *ds = ds_from_ist(ist);
    AVPacketSideData *sd;
    double rotation = DBL_MAX;
    int hflip = -1, vflip = -1;
    int hflip_set = 0, vflip_set = 0, rotation_set = 0;
    int32_t *buf;

    opt_match_per_stream_dbl(ist, &o->display_rotations, ctx, st, &rotation);
    opt_match_per_stream_int(ist, &o->display_hflips, ctx, st, &hflip);
    opt_match_per_stream_int(ist, &o->display_vflips, ctx, st, &vflip);

    rotation_set = rotation != DBL_MAX;
    hflip_set    = hflip != -1;
    vflip_set    = vflip != -1;

    if (!rotation_set && !hflip_set && !vflip_set)
        return 0;

    sd = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                 &st->codecpar->nb_coded_side_data,
                                 AV_PKT_DATA_DISPLAYMATRIX,
                                 sizeof(int32_t) * 9, 0);
    if (!sd) {
        av_log(ist, AV_LOG_FATAL, "Failed to generate a display matrix!\n");
        return AVERROR(ENOMEM);
    }

    buf = (int32_t *)sd->data;
    av_display_rotation_set(buf,
                            rotation_set ? -(rotation) : -0.0f);

    av_display_matrix_flip(buf,
                           hflip_set ? hflip : 0,
                           vflip_set ? vflip : 0);

    ds->force_display_matrix = 1;

    return 0;
}

static const char *input_stream_item_name(void *obj)
{
    const DemuxStream *ds = obj;

    return ds->log_name;
}

static const AVClass input_stream_class = {
    .class_name = "InputStream",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = input_stream_item_name,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

static DemuxStream *demux_stream_alloc(Demuxer *d, AVStream *st)
{
    const char *type_str = av_get_media_type_string(st->codecpar->codec_type);
    InputFile    *f = &d->f;
    DemuxStream *ds;

    ds = allocate_array_elem(&f->streams, sizeof(*ds), &f->nb_streams);
    if (!ds)
        return NULL;

    ds->sch_idx_stream = -1;
    ds->sch_idx_dec    = -1;

    ds->ist.st         = st;
    ds->ist.file       = f;
    ds->ist.index      = st->index;
    ds->ist.class      = &input_stream_class;

    snprintf(ds->log_name, sizeof(ds->log_name), "%cist#%d:%d/%s",
             type_str ? *type_str : '?', d->f.index, st->index,
             avcodec_get_name(st->codecpar->codec_id));

    return ds;
}

static int ist_add(const OptionsContext *o, Demuxer *d, AVStream *st, AVDictionary **opts_used)
{
    AVFormatContext *ic = d->f.ctx;
    AVCodecParameters *par = st->codecpar;
    DemuxStream *ds;
    InputStream *ist;
    const char *framerate = NULL, *hwaccel_device = NULL;
    const char *hwaccel = NULL;
    const char *apply_cropping = NULL;
    const char *hwaccel_output_format = NULL;
    const char *codec_tag = NULL;
    const char *bsfs = NULL;
    char *next;
    const char *discard_str = NULL;
    int ret;

    ds  = demux_stream_alloc(d, st);
    if (!ds)
        return AVERROR(ENOMEM);

    ist = &ds->ist;

    ds->discard     = 1;
    st->discard  = AVDISCARD_ALL;
    ds->first_dts   = AV_NOPTS_VALUE;
    ds->next_dts    = AV_NOPTS_VALUE;

    ds->dec_opts.time_base = st->time_base;

    ds->ts_scale = 1.0;
    opt_match_per_stream_dbl(ist, &o->ts_scale, ic, st, &ds->ts_scale);

    ds->autorotate = 1;
    opt_match_per_stream_int(ist, &o->autorotate, ic, st, &ds->autorotate);

    ds->apply_cropping = CROP_ALL;
    opt_match_per_stream_str(ist, &o->apply_cropping, ic, st, &apply_cropping);
    if (apply_cropping) {
        const AVOption opts[] = {
            { "apply_cropping", NULL, 0, AV_OPT_TYPE_INT,
                    { .i64 = CROP_ALL }, CROP_DISABLED, CROP_CONTAINER, AV_OPT_FLAG_DECODING_PARAM, .unit = "apply_cropping" },
                { "none",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = CROP_DISABLED  }, .unit = "apply_cropping" },
                { "all",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = CROP_ALL       }, .unit = "apply_cropping" },
                { "codec",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = CROP_CODEC     }, .unit = "apply_cropping" },
                { "container", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = CROP_CONTAINER }, .unit = "apply_cropping" },
            { NULL },
        };
        const AVClass class = {
            .class_name = "apply_cropping",
            .item_name  = av_default_item_name,
            .option     = opts,
            .version    = LIBAVUTIL_VERSION_INT,
        };
        const AVClass *pclass = &class;

        ret = av_opt_eval_int(&pclass, opts, apply_cropping, &ds->apply_cropping);
        if (ret < 0) {
            av_log(ist, AV_LOG_ERROR, "Invalid apply_cropping value '%s'.\n", apply_cropping);
            return ret;
        }
    }

    opt_match_per_stream_str(ist, &o->codec_tags, ic, st, &codec_tag);
    if (codec_tag) {
        uint32_t tag = strtol(codec_tag, &next, 0);
        if (*next) {
            uint8_t buf[4] = { 0 };
            memcpy(buf, codec_tag, FFMIN(sizeof(buf), strlen(codec_tag)));
            tag = AV_RL32(buf);
        }

        st->codecpar->codec_tag = tag;
    }

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = add_display_matrix_to_stream(o, ic, ist);
        if (ret < 0)
            return ret;

        opt_match_per_stream_str(ist, &o->hwaccels, ic, st, &hwaccel);
        opt_match_per_stream_str(ist, &o->hwaccel_output_formats, ic, st,
                                       &hwaccel_output_format);
        if (!hwaccel_output_format && hwaccel && !strcmp(hwaccel, "cuvid")) {
            av_log(ist, AV_LOG_WARNING,
                "WARNING: defaulting hwaccel_output_format to cuda for compatibility "
                "with old commandlines. This behaviour is DEPRECATED and will be removed "
                "in the future. Please explicitly set \"-hwaccel_output_format cuda\".\n");
            ds->dec_opts.hwaccel_output_format = AV_PIX_FMT_CUDA;
        } else if (!hwaccel_output_format && hwaccel && !strcmp(hwaccel, "qsv")) {
            av_log(ist, AV_LOG_WARNING,
                "WARNING: defaulting hwaccel_output_format to qsv for compatibility "
                "with old commandlines. This behaviour is DEPRECATED and will be removed "
                "in the future. Please explicitly set \"-hwaccel_output_format qsv\".\n");
            ds->dec_opts.hwaccel_output_format = AV_PIX_FMT_QSV;
        } else if (!hwaccel_output_format && hwaccel && !strcmp(hwaccel, "mediacodec")) {
            // There is no real AVHWFrameContext implementation. Set
            // hwaccel_output_format to avoid av_hwframe_transfer_data error.
            ds->dec_opts.hwaccel_output_format = AV_PIX_FMT_MEDIACODEC;
        } else if (hwaccel_output_format) {
            ds->dec_opts.hwaccel_output_format = av_get_pix_fmt(hwaccel_output_format);
            if (ds->dec_opts.hwaccel_output_format == AV_PIX_FMT_NONE) {
                av_log(ist, AV_LOG_FATAL, "Unrecognised hwaccel output "
                       "format: %s", hwaccel_output_format);
            }
        } else {
            ds->dec_opts.hwaccel_output_format = AV_PIX_FMT_NONE;
        }

        if (hwaccel) {
            // The NVDEC hwaccels use a CUDA device, so remap the name here.
            if (!strcmp(hwaccel, "nvdec") || !strcmp(hwaccel, "cuvid"))
                hwaccel = "cuda";

            if (!strcmp(hwaccel, "none"))
                ds->dec_opts.hwaccel_id = HWACCEL_NONE;
            else if (!strcmp(hwaccel, "auto"))
                ds->dec_opts.hwaccel_id = HWACCEL_AUTO;
            else {
                enum AVHWDeviceType type = av_hwdevice_find_type_by_name(hwaccel);
                if (type != AV_HWDEVICE_TYPE_NONE) {
                    ds->dec_opts.hwaccel_id = HWACCEL_GENERIC;
                    ds->dec_opts.hwaccel_device_type = type;
                }

                if (!ds->dec_opts.hwaccel_id) {
                    av_log(ist, AV_LOG_FATAL, "Unrecognized hwaccel: %s.\n",
                           hwaccel);
                    av_log(ist, AV_LOG_FATAL, "Supported hwaccels: ");
                    type = AV_HWDEVICE_TYPE_NONE;
                    while ((type = av_hwdevice_iterate_types(type)) !=
                           AV_HWDEVICE_TYPE_NONE)
                        av_log(ist, AV_LOG_FATAL, "%s ",
                               av_hwdevice_get_type_name(type));
                    av_log(ist, AV_LOG_FATAL, "\n");
                    return AVERROR(EINVAL);
                }
            }
        }

        opt_match_per_stream_str(ist, &o->hwaccel_devices, ic, st, &hwaccel_device);
        if (hwaccel_device) {
            ds->dec_opts.hwaccel_device = av_strdup(hwaccel_device);
            if (!ds->dec_opts.hwaccel_device)
                return AVERROR(ENOMEM);
        }
    }

    ret = choose_decoder(o, ist, ic, st, ds->dec_opts.hwaccel_id,
                         ds->dec_opts.hwaccel_device_type, &ist->dec);
    if (ret < 0)
        return ret;

    if (ist->dec) {
        ret = filter_codec_opts(o->g->codec_opts, ist->st->codecpar->codec_id,
                                ic, st, ist->dec, &ds->decoder_opts, opts_used);
        if (ret < 0)
            return ret;
    }

    ds->reinit_filters = -1;
    opt_match_per_stream_int(ist, &o->reinit_filters, ic, st, &ds->reinit_filters);

    ds->drop_changed = 0;
    opt_match_per_stream_int(ist, &o->drop_changed, ic, st, &ds->drop_changed);

    if (ds->drop_changed && ds->reinit_filters) {
        if (ds->reinit_filters > 0) {
            av_log(ist, AV_LOG_ERROR, "drop_changed and reinit_filters both enabled. These are mutually exclusive.\n");
            return AVERROR(EINVAL);
        }
        ds->reinit_filters = 0;
    }

    ist->user_set_discard = AVDISCARD_NONE;

    if ((o->video_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ||
        (o->audio_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ||
        (o->subtitle_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) ||
        (o->data_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA))
            ist->user_set_discard = AVDISCARD_ALL;

    opt_match_per_stream_str(ist, &o->discard, ic, st, &discard_str);
    if (discard_str) {
        ret = av_opt_set(ist->st, "discard", discard_str, 0);
        if (ret  < 0) {
            av_log(ist, AV_LOG_ERROR, "Error parsing discard %s.\n", discard_str);
            return ret;
        }
        ist->user_set_discard = ist->st->discard;
    }

    ds->dec_opts.flags |= DECODER_FLAG_BITEXACT * !!o->bitexact;

    av_dict_set_int(&ds->decoder_opts, "apply_cropping",
                    ds->apply_cropping && ds->apply_cropping != CROP_CONTAINER, 0);

    if (ds->force_display_matrix) {
        char buf[32];
        if (av_dict_get(ds->decoder_opts, "side_data_prefer_packet", NULL, 0))
            buf[0] = ',';
        else
            buf[0] = '\0';
        av_strlcat(buf, "displaymatrix", sizeof(buf));
        av_dict_set(&ds->decoder_opts, "side_data_prefer_packet", buf, AV_DICT_APPEND);
    }
    /* Attached pics are sparse, therefore we would not want to delay their decoding
     * till EOF. */
    if (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        av_dict_set(&ds->decoder_opts, "thread_type", "-frame", 0);

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        opt_match_per_stream_str(ist, &o->frame_rates, ic, st, &framerate);
        if (framerate) {
            ret = av_parse_video_rate(&ist->framerate, framerate);
            if (ret < 0) {
                av_log(ist, AV_LOG_ERROR, "Error parsing framerate %s.\n",
                       framerate);
                return ret;
            }
        }

#if FFMPEG_OPT_TOP
        ist->top_field_first = -1;
        opt_match_per_stream_int(ist, &o->top_field_first, ic, st, &ist->top_field_first);
#endif

        break;
    case AVMEDIA_TYPE_AUDIO: {
        const char *ch_layout_str = NULL;

        opt_match_per_stream_str(ist, &o->audio_ch_layouts, ic, st, &ch_layout_str);
        if (ch_layout_str) {
            AVChannelLayout ch_layout;
            ret = av_channel_layout_from_string(&ch_layout, ch_layout_str);
            if (ret < 0) {
                av_log(ist, AV_LOG_ERROR, "Error parsing channel layout %s.\n", ch_layout_str);
                return ret;
            }
            if (par->ch_layout.nb_channels <= 0 || par->ch_layout.nb_channels == ch_layout.nb_channels) {
                av_channel_layout_uninit(&par->ch_layout);
                par->ch_layout = ch_layout;
            } else {
                av_log(ist, AV_LOG_ERROR,
                    "Specified channel layout '%s' has %d channels, but input has %d channels.\n",
                    ch_layout_str, ch_layout.nb_channels, par->ch_layout.nb_channels);
                av_channel_layout_uninit(&ch_layout);
                return AVERROR(EINVAL);
            }
        } else {
            int guess_layout_max = INT_MAX;
            opt_match_per_stream_int(ist, &o->guess_layout_max, ic, st, &guess_layout_max);
            guess_input_channel_layout(ist, par, guess_layout_max);
        }
        break;
    }
    case AVMEDIA_TYPE_DATA:
    case AVMEDIA_TYPE_SUBTITLE: {
        const char *canvas_size = NULL;

        opt_match_per_stream_int(ist, &o->fix_sub_duration, ic, st, &ist->fix_sub_duration);
        opt_match_per_stream_str(ist, &o->canvas_sizes, ic, st, &canvas_size);
        if (canvas_size) {
            ret = av_parse_video_size(&par->width, &par->height,
                                      canvas_size);
            if (ret < 0) {
                av_log(ist, AV_LOG_FATAL, "Invalid canvas size: %s.\n", canvas_size);
                return ret;
            }
        }
        break;
    }
    case AVMEDIA_TYPE_ATTACHMENT:
    case AVMEDIA_TYPE_UNKNOWN:
        break;
    default: av_assert0(0);
    }

    ist->par = avcodec_parameters_alloc();
    if (!ist->par)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_copy(ist->par, par);
    if (ret < 0) {
        av_log(ist, AV_LOG_ERROR, "Error exporting stream parameters.\n");
        return ret;
    }

    if (ist->st->sample_aspect_ratio.num)
        ist->par->sample_aspect_ratio = ist->st->sample_aspect_ratio;

    opt_match_per_stream_str(ist, &o->bitstream_filters, ic, st, &bsfs);
    if (bsfs) {
        ret = av_bsf_list_parse_str(bsfs, &ds->bsf);
        if (ret < 0) {
            av_log(ist, AV_LOG_ERROR,
                   "Error parsing bitstream filter sequence '%s': %s\n",
                   bsfs, av_err2str(ret));
            return ret;
        }

        ret = avcodec_parameters_copy(ds->bsf->par_in, ist->par);
        if (ret < 0)
            return ret;
        ds->bsf->time_base_in = ist->st->time_base;

        ret = av_bsf_init(ds->bsf);
        if (ret < 0) {
            av_log(ist, AV_LOG_ERROR, "Error initializing bitstream filters: %s\n",
                   av_err2str(ret));
            return ret;
        }

        ret = avcodec_parameters_copy(ist->par, ds->bsf->par_out);
        if (ret < 0)
            return ret;
    }

    ds->codec_desc = avcodec_descriptor_get(ist->par->codec_id);

    return 0;
}

static const char *input_stream_group_item_name(void *obj)
{
    const DemuxStreamGroup *dsg = obj;

    return dsg->log_name;
}

static const AVClass input_stream_group_class = {
    .class_name = "InputStreamGroup",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = input_stream_group_item_name,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

static DemuxStreamGroup *demux_stream_group_alloc(Demuxer *d, AVStreamGroup *stg)
{
    InputFile    *f = &d->f;
    DemuxStreamGroup *dsg;

    dsg = allocate_array_elem(&f->stream_groups, sizeof(*dsg), &f->nb_stream_groups);
    if (!dsg)
        return NULL;

    dsg->istg.stg        = stg;
    dsg->istg.file       = f;
    dsg->istg.index      = stg->index;
    dsg->istg.class      = &input_stream_group_class;

    snprintf(dsg->log_name, sizeof(dsg->log_name), "istg#%d:%d/%s",
             d->f.index, stg->index, avformat_stream_group_name(stg->type));

    return dsg;
}

static int istg_parse_tile_grid(const OptionsContext *o, Demuxer *d, InputStreamGroup *istg)
{
    InputFile *f = &d->f;
    AVFormatContext *ic = d->f.ctx;
    AVStreamGroup *stg = istg->stg;
    const AVStreamGroupTileGrid *tg = stg->params.tile_grid;
    OutputFilterOptions opts;
    AVBPrint bp;
    char *graph_str;
    int autorotate = 1;
    const char *apply_cropping = NULL;
    int  ret;

    if (tg->nb_tiles == 1)
        return 0;

    memset(&opts, 0, sizeof(opts));

    opt_match_per_stream_group_int(istg, &o->autorotate, ic, stg, &autorotate);
    if (autorotate)
        opts.flags |= OFILTER_FLAG_AUTOROTATE;

    opts.flags |= OFILTER_FLAG_CROP;
    opt_match_per_stream_group_str(istg, &o->apply_cropping, ic, stg, &apply_cropping);
    if (apply_cropping) {
        char *p;
        int crop = strtol(apply_cropping, &p, 0);
        if (*p)
            return AVERROR(EINVAL);
        if (!crop)
            opts.flags &= ~OFILTER_FLAG_CROP;
    }

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (int i = 0; i < tg->nb_tiles; i++)
        av_bprintf(&bp, "[%d:g:%d:%d]", f->index, stg->index, tg->offsets[i].idx);
    av_bprintf(&bp, "xstack=inputs=%d:layout=", tg->nb_tiles);
    for (int i = 0; i < tg->nb_tiles - 1; i++)
        av_bprintf(&bp, "%d_%d|", tg->offsets[i].horizontal,
                                  tg->offsets[i].vertical);
    av_bprintf(&bp, "%d_%d:fill=0x%02X%02X%02X@0x%02X", tg->offsets[tg->nb_tiles - 1].horizontal,
                                                        tg->offsets[tg->nb_tiles - 1].vertical,
                                                        tg->background[0], tg->background[1],
                                                        tg->background[2], tg->background[3]);
    av_bprintf(&bp, "[%d:g:%d]", f->index, stg->index);
    ret = av_bprint_finalize(&bp, &graph_str);
    if (ret < 0)
        return ret;

    if (tg->coded_width != tg->width || tg->coded_height != tg->height) {
        opts.crop_top    = tg->vertical_offset;
        opts.crop_bottom = tg->coded_height - tg->height - tg->vertical_offset;
        opts.crop_left   = tg->horizontal_offset;
        opts.crop_right  = tg->coded_width - tg->width - tg->horizontal_offset;
    }

    for (int i = 0; i < tg->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &tg->coded_side_data[i];

        ret = av_packet_side_data_to_frame(&opts.side_data, &opts.nb_side_data, sd, 0);
        if (ret < 0 && ret != AVERROR(EINVAL))
            goto fail;
    }

    ret = fg_create(NULL, &graph_str, d->sch, &opts);
    if (ret < 0)
        goto fail;

    istg->fg = filtergraphs[nb_filtergraphs-1];
    istg->fg->is_internal = 1;

    ret = 0;
fail:
    if (ret < 0)
        av_freep(&graph_str);

    return ret;
}

static int istg_add(const OptionsContext *o, Demuxer *d, AVStreamGroup *stg)
{
    DemuxStreamGroup *dsg;
    InputStreamGroup *istg;
    int ret;

    dsg = demux_stream_group_alloc(d, stg);
    if (!dsg)
        return AVERROR(ENOMEM);

    istg = &dsg->istg;

    switch (stg->type) {
    case AV_STREAM_GROUP_PARAMS_TILE_GRID:
        ret = istg_parse_tile_grid(o, d, istg);
        if (ret < 0)
            return ret;
        break;
    default:
        break;
    }

    return 0;
}

static int is_windows_reserved_device_name(const char *f)
{
#if HAVE_DOS_PATHS
    for (const char *p = f; p && *p; ) {
        char stem[6], *s;
        av_strlcpy(stem, p, sizeof(stem));
        if ((s = strchr(stem, '.')))
            *s = 0;
        if ((s = strpbrk(stem, "123456789")))
            *s = '1';

        if( !av_strcasecmp(stem, "AUX") ||
            !av_strcasecmp(stem, "CON") ||
            !av_strcasecmp(stem, "NUL") ||
            !av_strcasecmp(stem, "PRN") ||
            !av_strcasecmp(stem, "COM1") ||
            !av_strcasecmp(stem, "LPT1")
        )
            return 1;

        p = strchr(p, '/');
        if (p)
            p++;
    }
#endif
    return 0;
}

static int safe_filename(const char *f, int allow_subdir)
{
    const char *start = f;

    if (!*f || is_windows_reserved_device_name(f))
        return 0;

    for (; *f; f++) {
        /* A-Za-z0-9_- */
        if (!((unsigned)((*f | 32) - 'a') < 26 ||
              (unsigned)(*f - '0') < 10 || *f == '_' || *f == '-')) {
            if (f == start)
                return 0;
            else if (allow_subdir && *f == '/')
                start = f + 1;
            else if (*f != '.')
                return 0;
        }
    }
    return 1;
}

static int dump_attachment(InputStream *ist, const char *filename)
{
    AVStream *st = ist->st;
    int ret;
    AVIOContext *out = NULL;
    const AVDictionaryEntry *e;

    if (!st->codecpar->extradata_size) {
        av_log(ist, AV_LOG_WARNING, "No extradata to dump.\n");
        return 0;
    }
    if (!*filename && (e = av_dict_get(st->metadata, "filename", NULL, 0))) {
        filename = e->value;
        if (!safe_filename(filename, 0)) {
            av_log(ist, AV_LOG_ERROR, "Filename %s is unsafe\n", filename);
            return AVERROR(EINVAL);
        }
    }
    if (!*filename) {
        av_log(ist, AV_LOG_FATAL, "No filename specified and no 'filename' tag");
        return AVERROR(EINVAL);
    }

    ret = assert_file_overwrite(filename);
    if (ret < 0)
        return ret;

    if ((ret = avio_open2(&out, filename, AVIO_FLAG_WRITE, &int_cb, NULL)) < 0) {
        av_log(ist, AV_LOG_FATAL, "Could not open file %s for writing.\n",
               filename);
        return ret;
    }

    avio_write(out, st->codecpar->extradata, st->codecpar->extradata_size);
    ret = avio_close(out);

    if (ret >= 0)
        av_log(ist, AV_LOG_INFO, "Wrote attachment (%d bytes) to '%s'\n",
               st->codecpar->extradata_size, filename);

    return ret;
}

static const char *input_file_item_name(void *obj)
{
    const Demuxer *d = obj;

    return d->log_name;
}

static const AVClass input_file_class = {
    .class_name = "InputFile",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = input_file_item_name,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

static Demuxer *demux_alloc(void)
{
    Demuxer *d = allocate_array_elem(&input_files, sizeof(*d), &nb_input_files);

    if (!d)
        return NULL;

    d->f.class = &input_file_class;
    d->f.index = nb_input_files - 1;

    snprintf(d->log_name, sizeof(d->log_name), "in#%d", d->f.index);

    return d;
}

int ifile_open(const OptionsContext *o, const char *filename, Scheduler *sch)
{
    Demuxer   *d;
    InputFile *f;
    AVFormatContext *ic;
    const AVInputFormat *file_iformat = NULL;
    int err, ret = 0;
    int64_t timestamp;
    AVDictionary *opts_used = NULL;
    const char*    video_codec_name = NULL;
    const char*    audio_codec_name = NULL;
    const char* subtitle_codec_name = NULL;
    const char*     data_codec_name = NULL;
    int scan_all_pmts_set = 0;

    int64_t start_time     = o->start_time;
    int64_t start_time_eof = o->start_time_eof;
    int64_t stop_time      = o->stop_time;
    int64_t recording_time = o->recording_time;

    d = demux_alloc();
    if (!d)
        return AVERROR(ENOMEM);

    f = &d->f;

    ret = sch_add_demux(sch, input_thread, d);
    if (ret < 0)
        return ret;
    d->sch = sch;

    if (stop_time != INT64_MAX && recording_time != INT64_MAX) {
        stop_time = INT64_MAX;
        av_log(d, AV_LOG_WARNING, "-t and -to cannot be used together; using -t.\n");
    }

    if (stop_time != INT64_MAX && recording_time == INT64_MAX) {
        int64_t start = start_time == AV_NOPTS_VALUE ? 0 : start_time;
        if (stop_time <= start) {
            av_log(d, AV_LOG_ERROR, "-to value smaller than -ss; aborting.\n");
            return AVERROR(EINVAL);
        } else {
            recording_time = stop_time - start;
        }
    }

    if (o->format) {
        if (!(file_iformat = av_find_input_format(o->format))) {
            av_log(d, AV_LOG_FATAL, "Unknown input format: '%s'\n", o->format);
            return AVERROR(EINVAL);
        }
    }

    if (!strcmp(filename, "-"))
        filename = "fd:";

    stdin_interaction &= strncmp(filename, "pipe:", 5) &&
                         strcmp(filename, "fd:") &&
                         strcmp(filename, "/dev/stdin");

    /* get default parameters from command line */
    ic = avformat_alloc_context();
    if (!ic)
        return AVERROR(ENOMEM);
    ic->name = av_strdup(d->log_name);
    if (o->audio_sample_rate.nb_opt) {
        av_dict_set_int(&o->g->format_opts, "sample_rate", o->audio_sample_rate.opt[o->audio_sample_rate.nb_opt - 1].u.i, 0);
    }
    if (o->audio_channels.nb_opt) {
        const AVClass *priv_class;
        if (file_iformat && (priv_class = file_iformat->priv_class) &&
            av_opt_find(&priv_class, "ch_layout", NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%dC", o->audio_channels.opt[o->audio_channels.nb_opt - 1].u.i);
            av_dict_set(&o->g->format_opts, "ch_layout", buf, 0);
        }
    }
    if (o->audio_ch_layouts.nb_opt) {
        const AVClass *priv_class;
        if (file_iformat && (priv_class = file_iformat->priv_class) &&
            av_opt_find(&priv_class, "ch_layout", NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&o->g->format_opts, "ch_layout", o->audio_ch_layouts.opt[o->audio_ch_layouts.nb_opt - 1].u.str, 0);
        }
    }
    if (o->frame_rates.nb_opt) {
        const AVClass *priv_class;
        /* set the format-level framerate option;
         * this is important for video grabbers, e.g. x11 */
        if (file_iformat && (priv_class = file_iformat->priv_class) &&
            av_opt_find(&priv_class, "framerate", NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&o->g->format_opts, "framerate",
                        o->frame_rates.opt[o->frame_rates.nb_opt - 1].u.str, 0);
        }
    }
    if (o->frame_sizes.nb_opt) {
        av_dict_set(&o->g->format_opts, "video_size", o->frame_sizes.opt[o->frame_sizes.nb_opt - 1].u.str, 0);
    }
    if (o->frame_pix_fmts.nb_opt)
        av_dict_set(&o->g->format_opts, "pixel_format", o->frame_pix_fmts.opt[o->frame_pix_fmts.nb_opt - 1].u.str, 0);

    video_codec_name    = opt_match_per_type_str(&o->codec_names, 'v');
    audio_codec_name    = opt_match_per_type_str(&o->codec_names, 'a');
    subtitle_codec_name = opt_match_per_type_str(&o->codec_names, 's');
    data_codec_name     = opt_match_per_type_str(&o->codec_names, 'd');

    if (video_codec_name)
        ret = err_merge(ret, find_codec(NULL, video_codec_name   , AVMEDIA_TYPE_VIDEO   , 0,
                                        &ic->video_codec));
    if (audio_codec_name)
        ret = err_merge(ret, find_codec(NULL, audio_codec_name   , AVMEDIA_TYPE_AUDIO   , 0,
                                        &ic->audio_codec));
    if (subtitle_codec_name)
        ret = err_merge(ret, find_codec(NULL, subtitle_codec_name, AVMEDIA_TYPE_SUBTITLE, 0,
                                        &ic->subtitle_codec));
    if (data_codec_name)
        ret = err_merge(ret, find_codec(NULL, data_codec_name    , AVMEDIA_TYPE_DATA,     0,
                                        &ic->data_codec));
    if (ret < 0) {
        avformat_free_context(ic);
        return ret;
    }

    ic->video_codec_id     = video_codec_name    ? ic->video_codec->id    : AV_CODEC_ID_NONE;
    ic->audio_codec_id     = audio_codec_name    ? ic->audio_codec->id    : AV_CODEC_ID_NONE;
    ic->subtitle_codec_id  = subtitle_codec_name ? ic->subtitle_codec->id : AV_CODEC_ID_NONE;
    ic->data_codec_id      = data_codec_name     ? ic->data_codec->id     : AV_CODEC_ID_NONE;

    ic->flags |= AVFMT_FLAG_NONBLOCK;
    if (o->bitexact)
        ic->flags |= AVFMT_FLAG_BITEXACT;
    ic->interrupt_callback = int_cb;

    if (!av_dict_get(o->g->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&o->g->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    /* open the input file with generic avformat function */
    err = avformat_open_input(&ic, filename, file_iformat, &o->g->format_opts);
    if (err < 0) {
        if (err != AVERROR_EXIT)
            av_log(d, AV_LOG_ERROR,
                   "Error opening input: %s\n", av_err2str(err));
        if (err == AVERROR_PROTOCOL_NOT_FOUND)
            av_log(d, AV_LOG_ERROR, "Did you mean file:%s?\n", filename);
        return err;
    }
    f->ctx = ic;

    av_strlcat(d->log_name, "/",               sizeof(d->log_name));
    av_strlcat(d->log_name, ic->iformat->name, sizeof(d->log_name));
    av_freep(&ic->name);
    ic->name = av_strdup(d->log_name);

    if (scan_all_pmts_set)
        av_dict_set(&o->g->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    remove_avoptions(&o->g->format_opts, o->g->codec_opts);

    ret = check_avoptions(o->g->format_opts);
    if (ret < 0)
        return ret;

    /* apply forced codec ids */
    for (int i = 0; i < ic->nb_streams; i++) {
        const AVCodec *dummy;
        ret = choose_decoder(o, f, ic, ic->streams[i], HWACCEL_NONE, AV_HWDEVICE_TYPE_NONE,
                             &dummy);
        if (ret < 0)
            return ret;
    }

    if (o->find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic->nb_streams;

        ret = setup_find_stream_info_opts(ic, o->g->codec_opts, &opts);
        if (ret < 0)
            return ret;

        /* If not enough info to get the stream parameters, we decode the
           first frames to get it. (used in mpeg case for example) */
        ret = avformat_find_stream_info(ic, opts);

        for (int i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (ret < 0) {
            av_log(d, AV_LOG_FATAL, "could not find codec parameters\n");
            if (ic->nb_streams == 0)
                return ret;
        }
    }

    if (start_time != AV_NOPTS_VALUE && start_time_eof != AV_NOPTS_VALUE) {
        av_log(d, AV_LOG_WARNING, "Cannot use -ss and -sseof both, using -ss\n");
        start_time_eof = AV_NOPTS_VALUE;
    }

    if (start_time_eof != AV_NOPTS_VALUE) {
        if (start_time_eof >= 0) {
            av_log(d, AV_LOG_ERROR, "-sseof value must be negative; aborting\n");
            return AVERROR(EINVAL);
        }
        if (ic->duration > 0) {
            start_time = start_time_eof + ic->duration;
            if (start_time < 0) {
                av_log(d, AV_LOG_WARNING, "-sseof value seeks to before start of file; ignored\n");
                start_time = AV_NOPTS_VALUE;
            }
        } else
            av_log(d, AV_LOG_WARNING, "Cannot use -sseof, file duration not known\n");
    }
    timestamp = (start_time == AV_NOPTS_VALUE) ? 0 : start_time;
    /* add the stream start time */
    if (!o->seek_timestamp && ic->start_time != AV_NOPTS_VALUE)
        timestamp += ic->start_time;

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t seek_timestamp = timestamp;

        if (!(ic->iformat->flags & AVFMT_SEEK_TO_PTS)) {
            int dts_heuristic = 0;
            for (int i = 0; i < ic->nb_streams; i++) {
                const AVCodecParameters *par = ic->streams[i]->codecpar;
                if (par->video_delay) {
                    dts_heuristic = 1;
                    break;
                }
            }
            if (dts_heuristic) {
                seek_timestamp -= 3*AV_TIME_BASE / 23;
            }
        }
        ret = avformat_seek_file(ic, -1, INT64_MIN, seek_timestamp, seek_timestamp, 0);
        if (ret < 0) {
            av_log(d, AV_LOG_WARNING, "could not seek to position %0.3f\n",
                   (double)timestamp / AV_TIME_BASE);
        }
    }

    f->start_time = start_time;
    d->recording_time = recording_time;
    f->input_sync_ref = o->input_sync_ref;
    f->input_ts_offset = o->input_ts_offset;
    f->ts_offset  = o->input_ts_offset - (copy_ts ? (start_at_zero && ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0) : timestamp);
    d->accurate_seek   = o->accurate_seek;
    d->loop = o->loop;
    d->nb_streams_warn = ic->nb_streams;

    d->duration        = (Timestamp){ .ts = 0,              .tb = (AVRational){ 1, 1 } };
    d->min_pts         = (Timestamp){ .ts = AV_NOPTS_VALUE, .tb = (AVRational){ 1, 1 } };
    d->max_pts         = (Timestamp){ .ts = AV_NOPTS_VALUE, .tb = (AVRational){ 1, 1 } };

    d->readrate = o->readrate ? o->readrate : 0.0;
    if (d->readrate < 0.0f) {
        av_log(d, AV_LOG_ERROR, "Option -readrate is %0.3f; it must be non-negative.\n", d->readrate);
        return AVERROR(EINVAL);
    }
    if (o->rate_emu) {
        if (d->readrate) {
            av_log(d, AV_LOG_WARNING, "Both -readrate and -re set. Using -readrate %0.3f.\n", d->readrate);
        } else
            d->readrate = 1.0f;
    }

    if (d->readrate) {
        d->readrate_initial_burst = o->readrate_initial_burst ? o->readrate_initial_burst : 0.5;
        if (d->readrate_initial_burst < 0.0) {
            av_log(d, AV_LOG_ERROR,
                   "Option -readrate_initial_burst is %0.3f; it must be non-negative.\n",
                   d->readrate_initial_burst);
            return AVERROR(EINVAL);
        }
        d->readrate_catchup = o->readrate_catchup ? o->readrate_catchup : d->readrate * 1.05;
        if (d->readrate_catchup < d->readrate) {
            av_log(d, AV_LOG_ERROR,
                   "Option -readrate_catchup is %0.3f; it must be at least equal to %0.3f.\n",
                   d->readrate_catchup, d->readrate);
            return AVERROR(EINVAL);
        }
    } else {
        if (o->readrate_initial_burst) {
            av_log(d, AV_LOG_WARNING, "Option -readrate_initial_burst ignored "
                   "since neither -readrate nor -re were given\n");
        }
        if (o->readrate_catchup) {
            av_log(d, AV_LOG_WARNING, "Option -readrate_catchup ignored "
                   "since neither -readrate nor -re were given\n");
        }
    }

    /* Add all the streams from the given input file to the demuxer */
    for (int i = 0; i < ic->nb_streams; i++) {
        ret = ist_add(o, d, ic->streams[i], &opts_used);
        if (ret < 0) {
            av_dict_free(&opts_used);
            return ret;
        }
    }

    /* Add all the stream groups from the given input file to the demuxer */
    for (int i = 0; i < ic->nb_stream_groups; i++) {
        ret = istg_add(o, d, ic->stream_groups[i]);
        if (ret < 0)
            return ret;
    }

    /* dump the file content */
    av_dump_format(ic, f->index, filename, 0);

    /* check if all codec options have been used */
    ret = check_avoptions_used(o->g->codec_opts, opts_used, d, 1);
    av_dict_free(&opts_used);
    if (ret < 0)
        return ret;

    for (int i = 0; i < o->dump_attachment.nb_opt; i++) {
        for (int j = 0; j < f->nb_streams; j++) {
            InputStream *ist = f->streams[j];

            if (check_stream_specifier(ic, ist->st, o->dump_attachment.opt[i].specifier) == 1) {
                ret = dump_attachment(ist, o->dump_attachment.opt[i].u.str);
                if (ret < 0)
                    return ret;
            }
        }
    }

    return 0;
}


/* ========== fftools/ffmpeg_enc.c ========== */

/*
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

#include <math.h>
#include <stdint.h>

#include "ffmpeg.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/dict.h"
#include "libavutil/display.h"
#include "libavutil/eval.h"
#include "libavutil/frame.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/avcodec.h"

typedef struct EncoderPriv {
    Encoder        e;

    void          *log_parent;
    char           log_name[32];

    // combined size of all the packets received from the encoder
    uint64_t data_size;

    // number of packets received from the encoder
    uint64_t packets_encoded;

    int opened;
    int attach_par;

    Scheduler      *sch;
    unsigned        sch_idx;
} EncoderPriv;

static EncoderPriv *ep_from_enc(Encoder *enc)
{
    return (EncoderPriv*)enc;
}

// data that is local to the decoder thread and not visible outside of it
typedef struct EncoderThread {
    AVFrame *frame;
    AVPacket  *pkt;
} EncoderThread;

void enc_free(Encoder **penc)
{
    Encoder *enc = *penc;

    if (!enc)
        return;

    if (enc->enc_ctx)
        av_freep(&enc->enc_ctx->stats_in);
    avcodec_free_context(&enc->enc_ctx);

    av_freep(penc);
}

static const char *enc_item_name(void *obj)
{
    const EncoderPriv *ep = obj;

    return ep->log_name;
}

static const AVClass enc_class = {
    .class_name                = "Encoder",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(EncoderPriv, log_parent),
    .item_name                 = enc_item_name,
};

int enc_alloc(Encoder **penc, const AVCodec *codec,
              Scheduler *sch, unsigned sch_idx, void *log_parent)
{
    EncoderPriv *ep;
    int ret = 0;

    *penc = NULL;

    ep = av_mallocz(sizeof(*ep));
    if (!ep)
        return AVERROR(ENOMEM);

    ep->e.class    = &enc_class;
    ep->log_parent = log_parent;

    ep->sch     = sch;
    ep->sch_idx = sch_idx;

    snprintf(ep->log_name, sizeof(ep->log_name), "enc:%s", codec->name);

    ep->e.enc_ctx = avcodec_alloc_context3(codec);
    if (!ep->e.enc_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    *penc = &ep->e;

    return 0;
fail:
    enc_free((Encoder**)&ep);
    return ret;
}

static int hw_device_setup_for_encode(Encoder *e, AVCodecContext *enc_ctx,
                                      AVBufferRef *frames_ref)
{
    const AVCodecHWConfig *config;
    HWDevice *dev = NULL;

    if (frames_ref &&
        ((AVHWFramesContext*)frames_ref->data)->format ==
        enc_ctx->pix_fmt) {
        // Matching format, will try to use hw_frames_ctx.
    } else {
        frames_ref = NULL;
    }

    for (int i = 0;; i++) {
        config = avcodec_get_hw_config(enc_ctx->codec, i);
        if (!config)
            break;

        if (frames_ref &&
            config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX &&
            (config->pix_fmt == AV_PIX_FMT_NONE ||
             config->pix_fmt == enc_ctx->pix_fmt)) {
            av_log(e, AV_LOG_VERBOSE, "Using input "
                   "frames context (format %s) with %s encoder.\n",
                   av_get_pix_fmt_name(enc_ctx->pix_fmt),
                   enc_ctx->codec->name);
            enc_ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
            if (!enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
            return 0;
        }

        if (!dev &&
            config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            dev = hw_device_get_by_type(config->device_type);
    }

    if (dev) {
        av_log(e, AV_LOG_VERBOSE, "Using device %s "
               "(type %s) with %s encoder.\n", dev->name,
               av_hwdevice_get_type_name(dev->type), enc_ctx->codec->name);
        enc_ctx->hw_device_ctx = av_buffer_ref(dev->device_ref);
        if (!enc_ctx->hw_device_ctx)
            return AVERROR(ENOMEM);
    } else {
        // No device required, or no device available.
    }
    return 0;
}

int enc_open(void *opaque, const AVFrame *frame)
{
    OutputStream *ost = opaque;
    InputStream *ist = ost->ist;
    Encoder              *e = ost->enc;
    EncoderPriv         *ep = ep_from_enc(e);
    AVCodecContext *enc_ctx = e->enc_ctx;
    Decoder            *dec = NULL;
    const AVCodec      *enc = enc_ctx->codec;
    OutputFile          *of = ost->file;
    FrameData *fd;
    int frame_samples = 0;
    int ret;

    if (ep->opened)
        return 0;

    // frame is always non-NULL for audio and video
    av_assert0(frame || (enc->type != AVMEDIA_TYPE_VIDEO && enc->type != AVMEDIA_TYPE_AUDIO));

    if (frame) {
        av_assert0(frame->opaque_ref);
        fd = (FrameData*)frame->opaque_ref->data;

        ret = clone_side_data(&enc_ctx->decoded_side_data, &enc_ctx->nb_decoded_side_data,
                              fd->side_data, fd->nb_side_data, AV_FRAME_SIDE_DATA_FLAG_UNIQUE);
        if (ret < 0)
            return ret;
    }

    if (ist)
        dec = ist->decoder;

    // the timebase is chosen by filtering code
    if (ost->type == AVMEDIA_TYPE_AUDIO || ost->type == AVMEDIA_TYPE_VIDEO) {
        enc_ctx->time_base      = frame->time_base;
        enc_ctx->framerate      = fd->frame_rate_filter;
    }

    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        av_assert0(frame->format != AV_SAMPLE_FMT_NONE &&
                   frame->sample_rate > 0 &&
                   frame->ch_layout.nb_channels > 0);
        enc_ctx->sample_fmt     = frame->format;
        enc_ctx->sample_rate    = frame->sample_rate;
        ret = av_channel_layout_copy(&enc_ctx->ch_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;

        if (ost->bits_per_raw_sample)
            enc_ctx->bits_per_raw_sample = ost->bits_per_raw_sample;
        else
            enc_ctx->bits_per_raw_sample = FFMIN(fd->bits_per_raw_sample,
                                                 av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
        break;

    case AVMEDIA_TYPE_VIDEO: {
        av_assert0(frame->format != AV_PIX_FMT_NONE &&
                   frame->width > 0 &&
                   frame->height > 0);
        enc_ctx->width  = frame->width;
        enc_ctx->height = frame->height;
        enc_ctx->sample_aspect_ratio =
            ost->frame_aspect_ratio.num ? // overridden by the -aspect cli option
            av_mul_q(ost->frame_aspect_ratio, (AVRational){ enc_ctx->height, enc_ctx->width }) :
            frame->sample_aspect_ratio;

        enc_ctx->pix_fmt = frame->format;

        if (ost->bits_per_raw_sample)
            enc_ctx->bits_per_raw_sample = ost->bits_per_raw_sample;
        else
            enc_ctx->bits_per_raw_sample = FFMIN(fd->bits_per_raw_sample,
                                                 av_pix_fmt_desc_get(enc_ctx->pix_fmt)->comp[0].depth);

        /**
         * The video color properties should always be in sync with the user-
         * requested values, since we forward them to the filter graph.
         */
        enc_ctx->color_range            = frame->color_range;
        enc_ctx->color_primaries        = frame->color_primaries;
        enc_ctx->color_trc              = frame->color_trc;
        enc_ctx->colorspace             = frame->colorspace;
        enc_ctx->alpha_mode             = frame->alpha_mode;

        /* Video properties which are not part of filter graph negotiation */
        if (enc_ctx->chroma_sample_location == AVCHROMA_LOC_UNSPECIFIED) {
            enc_ctx->chroma_sample_location = frame->chroma_location;
        } else if (enc_ctx->chroma_sample_location != frame->chroma_location &&
                   frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
            av_log(e, AV_LOG_WARNING,
                   "Requested chroma sample location '%s' does not match the "
                   "frame tagged sample location '%s'; result may be incorrect.\n",
                   av_chroma_location_name(enc_ctx->chroma_sample_location),
                   av_chroma_location_name(frame->chroma_location));
        }

        if (enc_ctx->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) ||
            (frame->flags & AV_FRAME_FLAG_INTERLACED)
#if FFMPEG_OPT_TOP
            || ost->top_field_first >= 0
#endif
            ) {
            int top_field_first =
#if FFMPEG_OPT_TOP
                ost->top_field_first >= 0 ?
                ost->top_field_first :
#endif
                !!(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);

            if (enc->id == AV_CODEC_ID_MJPEG)
                enc_ctx->field_order = top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
            else
                enc_ctx->field_order = top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
        } else
            enc_ctx->field_order = AV_FIELD_PROGRESSIVE;

        break;
        }
    case AVMEDIA_TYPE_SUBTITLE:
        enc_ctx->time_base = AV_TIME_BASE_Q;

        if (!enc_ctx->width) {
            enc_ctx->width     = ost->ist->par->width;
            enc_ctx->height    = ost->ist->par->height;
        }

        av_assert0(dec);
        if (dec->subtitle_header) {
            /* ASS code assumes this buffer is null terminated so add extra byte. */
            enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
            if (!enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(enc_ctx->subtitle_header, dec->subtitle_header,
                   dec->subtitle_header_size);
            enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }

        break;
    default:
        av_assert0(0);
        break;
    }

    if (ost->bitexact)
        enc_ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    if (enc->capabilities & AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE)
        enc_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    enc_ctx->flags |= AV_CODEC_FLAG_FRAME_DURATION;

    ret = hw_device_setup_for_encode(e, enc_ctx, frame ? frame->hw_frames_ctx : NULL);
    if (ret < 0) {
        av_log(e, AV_LOG_ERROR,
               "Encoding hardware device setup failed: %s\n", av_err2str(ret));
        return ret;
    }

    if ((ret = avcodec_open2(enc_ctx, enc, NULL)) < 0) {
        if (ret != AVERROR_EXPERIMENTAL)
            av_log(e, AV_LOG_ERROR, "Error while opening encoder - maybe "
                   "incorrect parameters such as bit_rate, rate, width or height.\n");
        return ret;
    }

    ep->opened = 1;

    if (enc_ctx->frame_size)
        frame_samples = enc_ctx->frame_size;

    if (enc_ctx->bit_rate && enc_ctx->bit_rate < 1000 &&
        enc_ctx->codec_id != AV_CODEC_ID_CODEC2 /* don't complain about 700 bit/s modes */)
        av_log(e, AV_LOG_WARNING, "The bitrate parameter is set too low."
                                    " It takes bits/s as argument, not kbits/s\n");

    ret = of_stream_init(of, ost, enc_ctx);
    if (ret < 0)
        return ret;

    return frame_samples;
}

static int check_recording_time(OutputStream *ost, int64_t ts, AVRational tb)
{
    OutputFile *of = ost->file;

    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ts, tb, of->recording_time, AV_TIME_BASE_Q) >= 0) {
        return 0;
    }
    return 1;
}

static int do_subtitle_out(OutputFile *of, OutputStream *ost, const AVSubtitle *sub,
                           AVPacket *pkt)
{
    Encoder *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i, ret;
    AVCodecContext *enc;
    int64_t pts;

    if (sub->pts == AV_NOPTS_VALUE) {
        av_log(e, AV_LOG_ERROR, "Subtitle packets must have a pts\n");
        return exit_on_error ? AVERROR(EINVAL) : 0;
    }
    if ((of->start_time != AV_NOPTS_VALUE && sub->pts < of->start_time))
        return 0;

    enc = e->enc_ctx;

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else if (enc->codec_id == AV_CODEC_ID_ASS)
        nb = FFMAX(sub->num_rects, 1);
    else
        nb = 1;

    /* shift timestamp to honor -ss and make check_recording_time() work with -t */
    pts = sub->pts;
    if (of->start_time != AV_NOPTS_VALUE)
        pts -= of->start_time;
    for (i = 0; i < nb; i++) {
        AVSubtitle local_sub = *sub;

        if (!check_recording_time(ost, pts, AV_TIME_BASE_Q))
            return AVERROR_EOF;

        ret = av_new_packet(pkt, subtitle_out_max_size);
        if (ret < 0)
            return AVERROR(ENOMEM);

        local_sub.pts = pts;
        // start_display_time is required to be 0
        local_sub.pts               += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE && i == 1)
            local_sub.num_rects = 0;
        else if (enc->codec_id == AV_CODEC_ID_ASS && sub->num_rects > 0) {
            local_sub.num_rects = 1;
            local_sub.rects += i;
        }

        e->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, pkt->data, pkt->size, &local_sub);
        if (subtitle_out_size < 0) {
            av_log(e, AV_LOG_FATAL, "Subtitle encoding failed\n");
            return subtitle_out_size;
        }

        av_shrink_packet(pkt, subtitle_out_size);
        pkt->time_base = AV_TIME_BASE_Q;
        pkt->pts       = sub->pts;
        pkt->duration = av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt->pts += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
            else
                pkt->pts += av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
        }
        pkt->dts = pkt->pts;

        ret = sch_enc_send(ep->sch, ep->sch_idx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

    return 0;
}

void enc_stats_write(OutputStream *ost, EncStats *es,
                     const AVFrame *frame, const AVPacket *pkt,
                     uint64_t frame_num)
{
    Encoder      *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    AVIOContext *io = es->io;
    AVRational   tb = frame ? frame->time_base : pkt->time_base;
    int64_t     pts = frame ? frame->pts : pkt->pts;

    AVRational  tbi = (AVRational){ 0, 1};
    int64_t    ptsi = INT64_MAX;

    const FrameData *fd = NULL;

    if (frame ? frame->opaque_ref : pkt->opaque_ref) {
        fd   = (const FrameData*)(frame ? frame->opaque_ref->data : pkt->opaque_ref->data);
        tbi  = fd->dec.tb;
        ptsi = fd->dec.pts;
    }

    pthread_mutex_lock(&es->lock);

    for (size_t i = 0; i < es->nb_components; i++) {
        const EncStatsComponent *c = &es->components[i];

        switch (c->type) {
        case ENC_STATS_LITERAL:         avio_write (io, c->str,     c->str_len);                    continue;
        case ENC_STATS_FILE_IDX:        avio_printf(io, "%d",       ost->file->index);              continue;
        case ENC_STATS_STREAM_IDX:      avio_printf(io, "%d",       ost->index);                    continue;
        case ENC_STATS_TIMEBASE:        avio_printf(io, "%d/%d",    tb.num, tb.den);                continue;
        case ENC_STATS_TIMEBASE_IN:     avio_printf(io, "%d/%d",    tbi.num, tbi.den);              continue;
        case ENC_STATS_PTS:             avio_printf(io, "%"PRId64,  pts);                           continue;
        case ENC_STATS_PTS_IN:          avio_printf(io, "%"PRId64,  ptsi);                          continue;
        case ENC_STATS_PTS_TIME:        avio_printf(io, "%g",       pts * av_q2d(tb));              continue;
        case ENC_STATS_PTS_TIME_IN:     avio_printf(io, "%g",       ptsi == INT64_MAX ?
                                                                    INFINITY : ptsi * av_q2d(tbi)); continue;
        case ENC_STATS_FRAME_NUM:       avio_printf(io, "%"PRIu64,  frame_num);                     continue;
        case ENC_STATS_FRAME_NUM_IN:    avio_printf(io, "%"PRIu64,  fd ? fd->dec.frame_num : -1);   continue;
        }

        if (frame) {
            switch (c->type) {
            case ENC_STATS_SAMPLE_NUM:  avio_printf(io, "%"PRIu64,  e->samples_encoded);            continue;
            case ENC_STATS_NB_SAMPLES:  avio_printf(io, "%d",       frame->nb_samples);             continue;
            default: av_assert0(0);
            }
        } else {
            switch (c->type) {
            case ENC_STATS_DTS:         avio_printf(io, "%"PRId64,  pkt->dts);                      continue;
            case ENC_STATS_DTS_TIME:    avio_printf(io, "%g",       pkt->dts * av_q2d(tb));         continue;
            case ENC_STATS_PKT_SIZE:    avio_printf(io, "%d",       pkt->size);                     continue;
            case ENC_STATS_KEYFRAME:    avio_write(io, (pkt->flags & AV_PKT_FLAG_KEY) ?
                                                       "K" : "N", 1);                               continue;
            case ENC_STATS_BITRATE: {
                double duration = FFMAX(pkt->duration, 1) * av_q2d(tb);
                avio_printf(io, "%g",  8.0 * pkt->size / duration);
                continue;
            }
            case ENC_STATS_AVG_BITRATE: {
                double duration = pkt->dts * av_q2d(tb);
                avio_printf(io, "%g",  duration > 0 ? 8.0 * ep->data_size / duration : -1.);
                continue;
            }
            default: av_assert0(0);
            }
        }
    }
    avio_w8(io, '\n');
    avio_flush(io);

    pthread_mutex_unlock(&es->lock);
}

static inline double psnr(double d)
{
    return -10.0 * log10(d);
}

static int update_video_stats(OutputStream *ost, const AVPacket *pkt, int write_vstats)
{
    Encoder        *e = ost->enc;
    EncoderPriv   *ep = ep_from_enc(e);
    const uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS,
                                                NULL);
    AVCodecContext *enc = e->enc_ctx;
    enum AVPictureType pict_type;
    int64_t frame_number;
    double ti1, bitrate, avg_bitrate;
    double psnr_val = -1;
    int quality;

    quality        = sd ? AV_RL32(sd) : -1;
    pict_type      = sd ? sd[4] : AV_PICTURE_TYPE_NONE;

    atomic_store(&ost->quality, quality);

    if ((enc->flags & AV_CODEC_FLAG_PSNR) && sd && sd[5]) {
        // FIXME the scaling assumes 8bit
        double error = AV_RL64(sd + 8) / (enc->width * enc->height * 255.0 * 255.0);
        if (error >= 0 && error <= 1)
            psnr_val = psnr(error);
    }

    if (!write_vstats)
        return 0;

    /* this is executed just the first time update_video_stats is called */
    if (!vstats_file) {
        vstats_file = fopen(vstats_filename, "w");
        if (!vstats_file) {
            perror("fopen");
            return AVERROR(errno);
        }
    }

    frame_number = ep->packets_encoded;
    if (vstats_version <= 1) {
        fprintf(vstats_file, "frame= %5"PRId64" q= %2.1f ", frame_number,
                quality / (float)FF_QP2LAMBDA);
    } else  {
        fprintf(vstats_file, "out= %2d st= %2d frame= %5"PRId64" q= %2.1f ",
                ost->file->index, ost->index, frame_number,
                quality / (float)FF_QP2LAMBDA);
    }

    if (psnr_val >= 0)
        fprintf(vstats_file, "PSNR= %6.2f ", psnr_val);

    fprintf(vstats_file,"f_size= %6d ", pkt->size);
    /* compute pts value */
    ti1 = pkt->dts * av_q2d(pkt->time_base);
    if (ti1 < 0.01)
        ti1 = 0.01;

    bitrate     = (pkt->size * 8) / av_q2d(enc->time_base) / 1000.0;
    avg_bitrate = (double)(ep->data_size * 8) / ti1 / 1000.0;
    fprintf(vstats_file, "s_size= %8.0fKiB time= %0.3f br= %7.1fkbits/s avg_br= %7.1fkbits/s ",
           (double)ep->data_size / 1024, ti1, bitrate, avg_bitrate);
    fprintf(vstats_file, "type= %c\n", av_get_picture_type_char(pict_type));

    return 0;
}

static int encode_frame(OutputFile *of, OutputStream *ost, AVFrame *frame,
                        AVPacket *pkt)
{
    Encoder            *e = ost->enc;
    EncoderPriv       *ep = ep_from_enc(e);
    AVCodecContext   *enc = e->enc_ctx;
    const char *type_desc = av_get_media_type_string(enc->codec_type);
    const char    *action = frame ? "encode" : "flush";
    int ret;

    if (frame) {
        FrameData *fd = frame_data(frame);

        if (!fd)
            return AVERROR(ENOMEM);

        fd->wallclock[LATENCY_PROBE_ENC_PRE] = av_gettime_relative();

        if (ost->enc_stats_pre.io)
            enc_stats_write(ost, &ost->enc_stats_pre, frame, NULL,
                            e->frames_encoded);

        e->frames_encoded++;
        e->samples_encoded += frame->nb_samples;

        if (debug_ts) {
            av_log(e, AV_LOG_INFO, "encoder <- type:%s "
                   "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
                   type_desc,
                   av_ts2str(frame->pts), av_ts2timestr(frame->pts, &enc->time_base),
                   enc->time_base.num, enc->time_base.den);
        }

        if (frame->sample_aspect_ratio.num && !ost->frame_aspect_ratio.num)
            enc->sample_aspect_ratio = frame->sample_aspect_ratio;
    }

    update_benchmark(NULL);

    ret = avcodec_send_frame(enc, frame);
    if (ret < 0 && !(ret == AVERROR_EOF && !frame)) {
        av_log(e, AV_LOG_ERROR, "Error submitting %s frame to the encoder\n",
               type_desc);
        return ret;
    }

    while (1) {
        FrameData *fd;

        av_packet_unref(pkt);

        ret = avcodec_receive_packet(enc, pkt);
        update_benchmark("%s_%s %d.%d", action, type_desc,
                         of->index, ost->index);

        pkt->time_base = enc->time_base;

        /* if two pass, output log on success and EOF */
        if ((ret >= 0 || ret == AVERROR_EOF) && ost->logfile && enc->stats_out)
            fprintf(ost->logfile, "%s", enc->stats_out);

        if (ret == AVERROR(EAGAIN)) {
            av_assert0(frame); // should never happen during flushing
            return 0;
        } else if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(e, AV_LOG_ERROR, "%s encoding failed\n", type_desc);
            return ret;
        }

        fd = packet_data(pkt);
        if (!fd)
            return AVERROR(ENOMEM);
        fd->wallclock[LATENCY_PROBE_ENC_POST] = av_gettime_relative();

        // attach stream parameters to first packet if requested
        avcodec_parameters_free(&fd->par_enc);
        if (ep->attach_par && !ep->packets_encoded) {
            fd->par_enc = avcodec_parameters_alloc();
            if (!fd->par_enc)
                return AVERROR(ENOMEM);

            ret = avcodec_parameters_from_context(fd->par_enc, enc);
            if (ret < 0)
                return ret;
        }

        pkt->flags |= AV_PKT_FLAG_TRUSTED;

        if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            ret = update_video_stats(ost, pkt, !!vstats_filename);
            if (ret < 0)
                return ret;
        }

        if (ost->enc_stats_post.io)
            enc_stats_write(ost, &ost->enc_stats_post, NULL, pkt,
                            ep->packets_encoded);

        if (debug_ts) {
            av_log(e, AV_LOG_INFO, "encoder -> type:%s "
                   "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s "
                   "duration:%s duration_time:%s\n",
                   type_desc,
                   av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &enc->time_base),
                   av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &enc->time_base),
                   av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &enc->time_base));
        }

        ep->data_size += pkt->size;

        ep->packets_encoded++;

        ret = sch_enc_send(ep->sch, ep->sch_idx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

    av_unreachable("encode_frame() loop should return");
}

static enum AVPictureType forced_kf_apply(void *logctx, KeyframeForceCtx *kf,
                                          const AVFrame *frame)
{
    double pts_time;

    if (kf->ref_pts == AV_NOPTS_VALUE)
        kf->ref_pts = frame->pts;

    pts_time = (frame->pts - kf->ref_pts) * av_q2d(frame->time_base);
    if (kf->index < kf->nb_pts &&
        av_compare_ts(frame->pts, frame->time_base, kf->pts[kf->index], AV_TIME_BASE_Q) >= 0) {
        kf->index++;
        goto force_keyframe;
    } else if (kf->pexpr) {
        double res;
        kf->expr_const_values[FKF_T] = pts_time;
        res = av_expr_eval(kf->pexpr,
                           kf->expr_const_values, NULL);
        av_log(logctx, AV_LOG_TRACE,
               "force_key_frame: n:%f n_forced:%f prev_forced_n:%f t:%f prev_forced_t:%f -> res:%f\n",
               kf->expr_const_values[FKF_N],
               kf->expr_const_values[FKF_N_FORCED],
               kf->expr_const_values[FKF_PREV_FORCED_N],
               kf->expr_const_values[FKF_T],
               kf->expr_const_values[FKF_PREV_FORCED_T],
               res);

        kf->expr_const_values[FKF_N] += 1;

        if (res) {
            kf->expr_const_values[FKF_PREV_FORCED_N] = kf->expr_const_values[FKF_N] - 1;
            kf->expr_const_values[FKF_PREV_FORCED_T] = kf->expr_const_values[FKF_T];
            kf->expr_const_values[FKF_N_FORCED]     += 1;
            goto force_keyframe;
        }
    } else if (kf->type == KF_FORCE_SOURCE && (frame->flags & AV_FRAME_FLAG_KEY)) {
        goto force_keyframe;
    } else if (kf->type == KF_FORCE_SCD_METADATA &&
               av_dict_get(frame->metadata, "lavfi.scd.time", NULL, 0)) {
        goto force_keyframe;
    }

    return AV_PICTURE_TYPE_NONE;

force_keyframe:
    av_log(logctx, AV_LOG_DEBUG, "Forced keyframe at time %f\n", pts_time);
    return AV_PICTURE_TYPE_I;
}

static int frame_encode(OutputStream *ost, AVFrame *frame, AVPacket *pkt)
{
    Encoder *e = ost->enc;
    OutputFile *of = ost->file;
    enum AVMediaType type = ost->type;

    if (type == AVMEDIA_TYPE_SUBTITLE) {
        const AVSubtitle *subtitle = frame && frame->buf[0] ?
                                     (AVSubtitle*)frame->buf[0]->data : NULL;

        // no flushing for subtitles
        return subtitle && subtitle->num_rects ?
               do_subtitle_out(of, ost, subtitle, pkt) : 0;
    }

    if (frame) {
        if (!check_recording_time(ost, frame->pts, frame->time_base))
            return AVERROR_EOF;

        if (type == AVMEDIA_TYPE_VIDEO) {
            frame->quality   = e->enc_ctx->global_quality;
            frame->pict_type = forced_kf_apply(e, &ost->kf, frame);

#if FFMPEG_OPT_TOP
            if (ost->top_field_first >= 0) {
                frame->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
                frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST * (!!ost->top_field_first);
            }
#endif
        } else {
            if (!(e->enc_ctx->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                e->enc_ctx->ch_layout.nb_channels != frame->ch_layout.nb_channels) {
                av_log(e, AV_LOG_ERROR,
                       "Audio channel count changed and encoder does not support parameter changes\n");
                return 0;
            }
        }
    }

    return encode_frame(of, ost, frame, pkt);
}

static void enc_thread_set_name(const OutputStream *ost)
{
    char name[16];
    snprintf(name, sizeof(name), "enc%d:%d:%s", ost->file->index, ost->index,
             ost->enc->enc_ctx->codec->name);
    ff_thread_setname(name);
}

static void enc_thread_uninit(EncoderThread *et)
{
    av_packet_free(&et->pkt);
    av_frame_free(&et->frame);

    memset(et, 0, sizeof(*et));
}

static int enc_thread_init(EncoderThread *et)
{
    memset(et, 0, sizeof(*et));

    et->frame = av_frame_alloc();
    if (!et->frame)
        goto fail;

    et->pkt = av_packet_alloc();
    if (!et->pkt)
        goto fail;

    return 0;

fail:
    enc_thread_uninit(et);
    return AVERROR(ENOMEM);
}

int encoder_thread(void *arg)
{
    OutputStream *ost = arg;
    Encoder        *e = ost->enc;
    EncoderPriv   *ep = ep_from_enc(e);
    EncoderThread et;
    int ret = 0, input_status = 0;
    int name_set = 0;

    ret = enc_thread_init(&et);
    if (ret < 0)
        goto finish;

    /* Open the subtitle encoders immediately. AVFrame-based encoders
     * are opened through a callback from the scheduler once they get
     * their first frame
     *
     * N.B.: because the callback is called from a different thread,
     * enc_ctx MUST NOT be accessed before sch_enc_receive() returns
     * for the first time for audio/video. */
    if (ost->type != AVMEDIA_TYPE_VIDEO && ost->type != AVMEDIA_TYPE_AUDIO) {
        ret = enc_open(ost, NULL);
        if (ret < 0)
            goto finish;
    }

    while (!input_status) {
        input_status = sch_enc_receive(ep->sch, ep->sch_idx, et.frame);
        if (input_status < 0) {
            if (input_status == AVERROR_EOF) {
                av_log(e, AV_LOG_VERBOSE, "Encoder thread received EOF\n");
                if (ep->opened)
                    break;

                av_log(e, AV_LOG_ERROR, "Could not open encoder before EOF\n");
                ret = AVERROR(EINVAL);
            } else {
                av_log(e, AV_LOG_ERROR, "Error receiving a frame for encoding: %s\n",
                       av_err2str(ret));
                ret = input_status;
            }
            goto finish;
        }

        if (!name_set) {
            enc_thread_set_name(ost);
            name_set = 1;
        }

        ret = frame_encode(ost, et.frame, et.pkt);

        av_packet_unref(et.pkt);
        av_frame_unref(et.frame);

        if (ret < 0) {
            if (ret == AVERROR_EOF)
                av_log(e, AV_LOG_VERBOSE, "Encoder returned EOF, finishing\n");
            else
                av_log(e, AV_LOG_ERROR, "Error encoding a frame: %s\n",
                       av_err2str(ret));
            break;
        }
    }

    // flush the encoder
    if (ret == 0 || ret == AVERROR_EOF) {
        ret = frame_encode(ost, NULL, et.pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            av_log(e, AV_LOG_ERROR, "Error flushing encoder: %s\n",
                   av_err2str(ret));
    }

    // EOF is normal thread termination
    if (ret == AVERROR_EOF)
        ret = 0;

finish:
    enc_thread_uninit(&et);

    return ret;
}

int enc_loopback(Encoder *enc)
{
    EncoderPriv *ep = ep_from_enc(enc);
    ep->attach_par = 1;
    return ep->sch_idx;
}


/* ========== fftools/ffmpeg_filter.c ========== */

/*
 * ffmpeg filter configuration
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

#include <stdint.h>

#include "ffmpeg.h"
#include "graphprint.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/downmix_info.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

typedef struct FilterGraphPriv {
    FilterGraph      fg;

    // name used for logging
    char             log_name[32];

    int              is_simple;
    // true when the filtergraph contains only meta filters
    // that do not modify the frame data
    int              is_meta;
    // source filters are present in the graph
    int              have_sources;
    int              disable_conversions;

    unsigned         nb_outputs_done;

    int              nb_threads;

    // frame for temporarily holding output from the filtergraph
    AVFrame         *frame;
    // frame for sending output to the encoder
    AVFrame         *frame_enc;

    Scheduler       *sch;
    unsigned         sch_idx;
} FilterGraphPriv;

static FilterGraphPriv *fgp_from_fg(FilterGraph *fg)
{
    return (FilterGraphPriv*)fg;
}

static const FilterGraphPriv *cfgp_from_cfg(const FilterGraph *fg)
{
    return (const FilterGraphPriv*)fg;
}

// data that is local to the filter thread and not visible outside of it
typedef struct FilterGraphThread {
    AVFilterGraph   *graph;

    AVFrame         *frame;

    // Temporary buffer for output frames, since on filtergraph reset
    // we cannot send them to encoders immediately.
    // The output index is stored in frame opaque.
    AVFifo          *frame_queue_out;

    // index of the next input to request from the scheduler
    unsigned         next_in;
    // set to 1 after at least one frame passed through this output
    int              got_frame;

    // EOF status of each input/output, as received by the thread
    uint8_t         *eof_in;
    uint8_t         *eof_out;
} FilterGraphThread;

typedef struct InputFilterPriv {
    InputFilter         ifilter;

    InputFilterOptions  opts;

    // used to hold submitted input
    AVFrame            *frame;

    // For inputs bound to a filtergraph output
    OutputFilter       *ofilter_src;

    // source data type: AVMEDIA_TYPE_SUBTITLE for sub2video,
    // same as type otherwise
    enum AVMediaType    type_src;

    int                 eof;
    int                 bound;
    int                 drop_warned;
    uint64_t            nb_dropped;

    // parameters configured for this input
    int                 format;

    int                 width, height;
    AVRational          sample_aspect_ratio;
    enum AVColorSpace   color_space;
    enum AVColorRange   color_range;
    enum AVAlphaMode    alpha_mode;

    int                 sample_rate;
    AVChannelLayout     ch_layout;

    AVRational          time_base;

    AVFrameSideData   **side_data;
    int                 nb_side_data;

    AVFifo             *frame_queue;

    AVBufferRef        *hw_frames_ctx;

    int                 displaymatrix_present;
    int                 displaymatrix_applied;
    int32_t             displaymatrix[9];

    int                 downmixinfo_present;
    AVDownmixInfo       downmixinfo;

    struct {
        AVFrame *frame;

        int64_t last_pts;
        int64_t end_pts;

        /// marks if sub2video_update should force an initialization
        unsigned int initialize;
    } sub2video;
} InputFilterPriv;

static InputFilterPriv *ifp_from_ifilter(InputFilter *ifilter)
{
    return (InputFilterPriv*)ifilter;
}

typedef struct FPSConvContext {
    AVFrame          *last_frame;
    /* number of frames emitted by the video-encoding sync code */
    int64_t           frame_number;
    /* history of nb_frames_prev, i.e. the number of times the
     * previous frame was duplicated by vsync code in recent
     * do_video_out() calls */
    int64_t           frames_prev_hist[3];

    uint64_t          dup_warning;

    int               last_dropped;
    int               dropped_keyframe;

    enum VideoSyncMethod vsync_method;

    AVRational        framerate;
    AVRational        framerate_max;
    const AVRational *framerate_supported;
    int               framerate_clip;
} FPSConvContext;

typedef struct OutputFilterPriv {
    OutputFilter            ofilter;

    void                   *log_parent;
    char                    log_name[32];

    int                     needed;

    /* desired output stream properties */
    int                     format;
    int                     width, height;
    int                     sample_rate;
    AVChannelLayout         ch_layout;
    enum AVColorSpace       color_space;
    enum AVColorRange       color_range;
    enum AVAlphaMode        alpha_mode;

    unsigned                crop_top;
    unsigned                crop_bottom;
    unsigned                crop_left;
    unsigned                crop_right;

    AVFrameSideData       **side_data;
    int                     nb_side_data;

    // time base in which the output is sent to our downstream
    // does not need to match the filtersink's timebase
    AVRational              tb_out;
    // at least one frame with the above timebase was sent
    // to our downstream, so it cannot change anymore
    int                     tb_out_locked;

    AVRational              sample_aspect_ratio;

    AVDictionary           *sws_opts;
    AVDictionary           *swr_opts;

    // those are only set if no format is specified and the encoder gives us multiple options
    // They point directly to the relevant lists of the encoder.
    union {
        const enum AVPixelFormat *pix_fmts;
        const enum AVSampleFormat *sample_fmts;
    };
    const AVChannelLayout  *ch_layouts;
    const int              *sample_rates;
    const enum AVColorSpace *color_spaces;
    const enum AVColorRange *color_ranges;
    const enum AVAlphaMode *alpha_modes;

    int32_t                 displaymatrix[9];

    AVRational              enc_timebase;
    int64_t                 trim_start_us;
    int64_t                 trim_duration_us;
    // offset for output timestamps, in AV_TIME_BASE_Q
    int64_t                 ts_offset;
    int64_t                 next_pts;
    FPSConvContext          fps;

    unsigned                flags;
} OutputFilterPriv;

static OutputFilterPriv *ofp_from_ofilter(OutputFilter *ofilter)
{
    return (OutputFilterPriv*)ofilter;
}

typedef struct FilterCommand {
    char *target;
    char *command;
    char *arg;

    double time;
    int    all_filters;
} FilterCommand;

static void filter_command_free(void *opaque, uint8_t *data)
{
    FilterCommand *fc = (FilterCommand*)data;

    av_freep(&fc->target);
    av_freep(&fc->command);
    av_freep(&fc->arg);

    av_free(data);
}

static int sub2video_get_blank_frame(InputFilterPriv *ifp)
{
    AVFrame *frame = ifp->sub2video.frame;
    int ret;

    av_frame_unref(frame);

    frame->width  = ifp->width;
    frame->height = ifp->height;
    frame->format = ifp->format;
    frame->colorspace = ifp->color_space;
    frame->color_range = ifp->color_range;
    frame->alpha_mode = ifp->alpha_mode;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        return ret;

    memset(frame->data[0], 0, frame->height * frame->linesize[0]);

    return 0;
}

static void sub2video_copy_rect(uint8_t *dst, int dst_linesize, int w, int h,
                                AVSubtitleRect *r)
{
    uint32_t *pal, *dst2;
    uint8_t *src, *src2;
    int x, y;

    if (r->type != SUBTITLE_BITMAP) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: non-bitmap subtitle\n");
        return;
    }
    if (r->x < 0 || r->x + r->w > w || r->y < 0 || r->y + r->h > h) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: rectangle (%d %d %d %d) overflowing %d %d\n",
            r->x, r->y, r->w, r->h, w, h
        );
        return;
    }

    dst += r->y * dst_linesize + r->x * 4;
    src = r->data[0];
    pal = (uint32_t *)r->data[1];
    for (y = 0; y < r->h; y++) {
        dst2 = (uint32_t *)dst;
        src2 = src;
        for (x = 0; x < r->w; x++)
            *(dst2++) = pal[*(src2++)];
        dst += dst_linesize;
        src += r->linesize[0];
    }
}

static void sub2video_push_ref(InputFilterPriv *ifp, int64_t pts)
{
    AVFrame *frame = ifp->sub2video.frame;
    int ret;

    av_assert1(frame->data[0]);
    ifp->sub2video.last_pts = frame->pts = pts;
    ret = av_buffersrc_add_frame_flags(ifp->ifilter.filter, frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF |
                                       AV_BUFFERSRC_FLAG_PUSH);
    if (ret != AVERROR_EOF && ret < 0)
        av_log(ifp->ifilter.graph, AV_LOG_WARNING,
               "Error while add the frame to buffer source(%s).\n",
               av_err2str(ret));
}

static void sub2video_update(InputFilterPriv *ifp, int64_t heartbeat_pts,
                             const AVSubtitle *sub)
{
    AVFrame *frame = ifp->sub2video.frame;
    int8_t *dst;
    int     dst_linesize;
    int num_rects;
    int64_t pts, end_pts;

    if (sub) {
        pts       = av_rescale_q(sub->pts + sub->start_display_time * 1000LL,
                                 AV_TIME_BASE_Q, ifp->time_base);
        end_pts   = av_rescale_q(sub->pts + sub->end_display_time   * 1000LL,
                                 AV_TIME_BASE_Q, ifp->time_base);
        num_rects = sub->num_rects;
    } else {
        /* If we are initializing the system, utilize current heartbeat
           PTS as the start time, and show until the following subpicture
           is received. Otherwise, utilize the previous subpicture's end time
           as the fall-back value. */
        pts       = ifp->sub2video.initialize ?
                    heartbeat_pts : ifp->sub2video.end_pts;
        end_pts   = INT64_MAX;
        num_rects = 0;
    }
    if (sub2video_get_blank_frame(ifp) < 0) {
        av_log(ifp->ifilter.graph, AV_LOG_ERROR,
               "Impossible to get a blank canvas.\n");
        return;
    }
    dst          = frame->data    [0];
    dst_linesize = frame->linesize[0];
    for (int i = 0; i < num_rects; i++)
        sub2video_copy_rect(dst, dst_linesize, frame->width, frame->height, sub->rects[i]);
    sub2video_push_ref(ifp, pts);
    ifp->sub2video.end_pts = end_pts;
    ifp->sub2video.initialize = 0;
}

/* Define a function for appending a list of allowed formats
 * to an AVBPrint. If nonempty, the list will have a header. */
#define DEF_CHOOSE_FORMAT(name, type, var, supported_list, none, printf_format, get_name) \
static void choose_ ## name (OutputFilterPriv *ofp, AVBPrint *bprint)          \
{                                                                              \
    if (ofp->var == none && !ofp->supported_list)                              \
        return;                                                                \
    av_bprintf(bprint, #name "=");                                             \
    if (ofp->var != none) {                                                    \
        av_bprintf(bprint, printf_format, get_name(ofp->var));                 \
    } else {                                                                   \
        const type *p;                                                         \
                                                                               \
        for (p = ofp->supported_list; *p != none; p++) {                       \
            av_bprintf(bprint, printf_format "|", get_name(*p));               \
        }                                                                      \
        if (bprint->len > 0)                                                   \
            bprint->str[--bprint->len] = '\0';                                 \
    }                                                                          \
    av_bprint_chars(bprint, ':', 1);                                           \
}

DEF_CHOOSE_FORMAT(pix_fmts, enum AVPixelFormat, format, pix_fmts,
                  AV_PIX_FMT_NONE, "%s", av_get_pix_fmt_name)

DEF_CHOOSE_FORMAT(sample_fmts, enum AVSampleFormat, format, sample_fmts,
                  AV_SAMPLE_FMT_NONE, "%s", av_get_sample_fmt_name)

DEF_CHOOSE_FORMAT(sample_rates, int, sample_rate, sample_rates, 0,
                  "%d", )

DEF_CHOOSE_FORMAT(color_spaces, enum AVColorSpace, color_space, color_spaces,
                  AVCOL_SPC_UNSPECIFIED, "%s", av_color_space_name);

DEF_CHOOSE_FORMAT(color_ranges, enum AVColorRange, color_range, color_ranges,
                  AVCOL_RANGE_UNSPECIFIED, "%s", av_color_range_name);

DEF_CHOOSE_FORMAT(alpha_modes, enum AVAlphaMode, alpha_mode, alpha_modes,
                  AVALPHA_MODE_UNSPECIFIED, "%s", av_alpha_mode_name);

static void choose_channel_layouts(OutputFilterPriv *ofp, AVBPrint *bprint)
{
    if (av_channel_layout_check(&ofp->ch_layout)) {
        av_bprintf(bprint, "channel_layouts=");
        av_channel_layout_describe_bprint(&ofp->ch_layout, bprint);
    } else if (ofp->ch_layouts) {
        const AVChannelLayout *p;

        av_bprintf(bprint, "channel_layouts=");
        for (p = ofp->ch_layouts; p->nb_channels; p++) {
            av_channel_layout_describe_bprint(p, bprint);
            av_bprintf(bprint, "|");
        }
        if (bprint->len > 0)
            bprint->str[--bprint->len] = '\0';
    } else
        return;
    av_bprint_chars(bprint, ':', 1);
}

static int read_binary(void *logctx, const char *path,
                       uint8_t **data, int *len)
{
    AVIOContext *io = NULL;
    int64_t fsize;
    int ret;

    *data = NULL;
    *len  = 0;

    ret = avio_open2(&io, path, AVIO_FLAG_READ, &int_cb, NULL);
    if (ret < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot open file '%s': %s\n",
               path, av_err2str(ret));
        return ret;
    }

    fsize = avio_size(io);
    if (fsize < 0 || fsize > INT_MAX) {
        av_log(logctx, AV_LOG_ERROR, "Cannot obtain size of file %s\n", path);
        ret = AVERROR(EIO);
        goto fail;
    }

    *data = av_malloc(fsize);
    if (!*data) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = avio_read(io, *data, fsize);
    if (ret != fsize) {
        av_log(logctx, AV_LOG_ERROR, "Error reading file %s\n", path);
        ret = ret < 0 ? ret : AVERROR(EIO);
        goto fail;
    }

    *len = fsize;

    ret = 0;
fail:
    avio_close(io);
    if (ret < 0) {
        av_freep(data);
        *len = 0;
    }
    return ret;
}

static int filter_opt_apply(void *logctx, AVFilterContext *f,
                            const char *key, const char *val)
{
    const AVOption *o = NULL;
    int ret;

    ret = av_opt_set(f, key, val, AV_OPT_SEARCH_CHILDREN);
    if (ret >= 0)
        return 0;

    if (ret == AVERROR_OPTION_NOT_FOUND && key[0] == '/')
        o = av_opt_find(f, key + 1, NULL, 0, AV_OPT_SEARCH_CHILDREN);
    if (!o)
        goto err_apply;

    // key is a valid option name prefixed with '/'
    // interpret value as a path from which to load the actual option value
    key++;

    if (o->type == AV_OPT_TYPE_BINARY) {
        uint8_t *data;
        int      len;

        ret = read_binary(logctx, val, &data, &len);
        if (ret < 0)
            goto err_load;

        ret = av_opt_set_bin(f, key, data, len, AV_OPT_SEARCH_CHILDREN);
        av_freep(&data);
    } else {
        char *data = read_file_to_string(val);
        if (!data) {
            ret = AVERROR(EIO);
            goto err_load;
        }

        ret = av_opt_set(f, key, data, AV_OPT_SEARCH_CHILDREN);
        av_freep(&data);
    }
    if (ret < 0)
        goto err_apply;

    return 0;

err_apply:
    av_log(logctx, AV_LOG_ERROR,
           "Error applying option '%s' to filter '%s': %s\n",
           key, f->filter->name, av_err2str(ret));
    return ret;
err_load:
    av_log(logctx, AV_LOG_ERROR,
           "Error loading value for option '%s' from file '%s'\n",
           key, val);
    return ret;
}

static int graph_opts_apply(void *logctx, AVFilterGraphSegment *seg)
{
    for (size_t i = 0; i < seg->nb_chains; i++) {
        AVFilterChain *ch = seg->chains[i];

        for (size_t j = 0; j < ch->nb_filters; j++) {
            AVFilterParams *p = ch->filters[j];
            const AVDictionaryEntry *e = NULL;

            av_assert0(p->filter);

            while ((e = av_dict_iterate(p->opts, e))) {
                int ret = filter_opt_apply(logctx, p->filter, e->key, e->value);
                if (ret < 0)
                    return ret;
            }

            av_dict_free(&p->opts);
        }
    }

    return 0;
}

static int graph_parse(void *logctx,
                       AVFilterGraph *graph, const char *desc,
                       AVFilterInOut **inputs, AVFilterInOut **outputs,
                       AVBufferRef *hw_device)
{
    AVFilterGraphSegment *seg;
    int ret;

    *inputs  = NULL;
    *outputs = NULL;

    ret = avfilter_graph_segment_parse(graph, desc, 0, &seg);
    if (ret < 0)
        return ret;

    ret = avfilter_graph_segment_create_filters(seg, 0);
    if (ret < 0)
        goto fail;

    if (hw_device) {
        for (int i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *f = graph->filters[i];

            if (!(f->filter->flags & AVFILTER_FLAG_HWDEVICE))
                continue;
            f->hw_device_ctx = av_buffer_ref(hw_device);
            if (!f->hw_device_ctx) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
    }

    ret = graph_opts_apply(logctx, seg);
    if (ret < 0)
        goto fail;

    ret = avfilter_graph_segment_apply(seg, 0, inputs, outputs);

fail:
    avfilter_graph_segment_free(&seg);
    return ret;
}

// Filters can be configured only if the formats of all inputs are known.
static int ifilter_has_all_input_formats(FilterGraph *fg)
{
    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilterPriv *ifp = ifp_from_ifilter(fg->inputs[i]);
        if (ifp->format < 0)
            return 0;
    }
    return 1;
}

static int filter_thread(void *arg);

static char *describe_filter_link(FilterGraph *fg, AVFilterInOut *inout, int in)
{
    AVFilterContext *ctx = inout->filter_ctx;
    AVFilterPad *pads = in ? ctx->input_pads  : ctx->output_pads;
    int       nb_pads = in ? ctx->nb_inputs   : ctx->nb_outputs;

    if (nb_pads > 1)
        return av_strdup(ctx->filter->name);
    return av_asprintf("%s:%s", ctx->filter->name,
                       avfilter_pad_get_name(pads, inout->pad_idx));
}

static const char *ofilter_item_name(void *obj)
{
    OutputFilterPriv *ofp = obj;
    return ofp->log_name;
}

static const AVClass ofilter_class = {
    .class_name                = "OutputFilter",
    .version                   = LIBAVUTIL_VERSION_INT,
    .item_name                 = ofilter_item_name,
    .parent_log_context_offset = offsetof(OutputFilterPriv, log_parent),
    .category                  = AV_CLASS_CATEGORY_FILTER,
};

static OutputFilter *ofilter_alloc(FilterGraph *fg, enum AVMediaType type)
{
    OutputFilterPriv *ofp;
    OutputFilter *ofilter;

    ofp = allocate_array_elem(&fg->outputs, sizeof(*ofp), &fg->nb_outputs);
    if (!ofp)
        return NULL;

    ofilter           = &ofp->ofilter;
    ofilter->class    = &ofilter_class;
    ofp->log_parent   = fg;
    ofilter->graph    = fg;
    ofilter->type     = type;
    ofp->format       = -1;
    ofp->color_space  = AVCOL_SPC_UNSPECIFIED;
    ofp->color_range  = AVCOL_RANGE_UNSPECIFIED;
    ofp->alpha_mode   = AVALPHA_MODE_UNSPECIFIED;
    ofilter->index    = fg->nb_outputs - 1;

    snprintf(ofp->log_name, sizeof(ofp->log_name), "%co%d",
             av_get_media_type_string(type)[0], ofilter->index);

    return ofilter;
}

static int ifilter_bind_ist(InputFilter *ifilter, InputStream *ist,
                            const ViewSpecifier *vs)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    FilterGraphPriv *fgp = fgp_from_fg(ifilter->graph);
    SchedulerNode src;
    int ret;

    av_assert0(!ifp->bound);
    ifp->bound = 1;

    if (ifilter->type != ist->par->codec_type &&
        !(ifilter->type == AVMEDIA_TYPE_VIDEO && ist->par->codec_type == AVMEDIA_TYPE_SUBTITLE)) {
        av_log(fgp, AV_LOG_ERROR, "Tried to connect %s stream to %s filtergraph input\n",
               av_get_media_type_string(ist->par->codec_type), av_get_media_type_string(ifilter->type));
        return AVERROR(EINVAL);
    }

    ifp->type_src        = ist->st->codecpar->codec_type;

    ifp->opts.fallback = av_frame_alloc();
    if (!ifp->opts.fallback)
        return AVERROR(ENOMEM);

    ret = ist_filter_add(ist, ifilter, filtergraph_is_simple(ifilter->graph),
                         vs, &ifp->opts, &src);
    if (ret < 0)
        return ret;

    ifilter->input_name = av_strdup(ifp->opts.name);
    if (!ifilter->input_name)
        return AVERROR(EINVAL);

    ret = sch_connect(fgp->sch,
                      src, SCH_FILTER_IN(fgp->sch_idx, ifilter->index));
    if (ret < 0)
        return ret;

    if (ifp->type_src == AVMEDIA_TYPE_SUBTITLE) {
        ifp->sub2video.frame = av_frame_alloc();
        if (!ifp->sub2video.frame)
            return AVERROR(ENOMEM);

        ifp->width  = ifp->opts.sub2video_width;
        ifp->height = ifp->opts.sub2video_height;

        /* rectangles are AV_PIX_FMT_PAL8, but we have no guarantee that the
           palettes for all rectangles are identical or compatible */
        ifp->format = AV_PIX_FMT_RGB32;

        ifp->time_base = AV_TIME_BASE_Q;

        av_log(fgp, AV_LOG_VERBOSE, "sub2video: using %dx%d canvas\n",
               ifp->width, ifp->height);
    }

    return 0;
}

static int ifilter_bind_dec(InputFilterPriv *ifp, Decoder *dec,
                            const ViewSpecifier *vs)
{
    FilterGraphPriv *fgp = fgp_from_fg(ifp->ifilter.graph);
    SchedulerNode src;
    int ret;

    av_assert0(!ifp->bound);
    ifp->bound = 1;

    if (ifp->ifilter.type != dec->type) {
        av_log(fgp, AV_LOG_ERROR, "Tried to connect %s decoder to %s filtergraph input\n",
               av_get_media_type_string(dec->type), av_get_media_type_string(ifp->ifilter.type));
        return AVERROR(EINVAL);
    }

    ifp->type_src = ifp->ifilter.type;

    ret = dec_filter_add(dec, &ifp->ifilter, &ifp->opts, vs, &src);
    if (ret < 0)
        return ret;

    ifp->ifilter.input_name = av_strdup(ifp->opts.name);
    if (!ifp->ifilter.input_name)
        return AVERROR(EINVAL);

    ret = sch_connect(fgp->sch, src, SCH_FILTER_IN(fgp->sch_idx, ifp->ifilter.index));
    if (ret < 0)
        return ret;

    return 0;
}

static int set_channel_layout(OutputFilterPriv *f, const AVChannelLayout *layouts_allowed,
                              const AVChannelLayout *layout_requested)
{
    int i, err;

    if (layout_requested->order != AV_CHANNEL_ORDER_UNSPEC) {
        /* Pass the layout through for all orders but UNSPEC */
        err = av_channel_layout_copy(&f->ch_layout, layout_requested);
        if (err < 0)
            return err;
        return 0;
    }

    /* Requested layout is of order UNSPEC */
    if (!layouts_allowed) {
        /* Use the default native layout for the requested amount of channels when the
           encoder doesn't have a list of supported layouts */
        av_channel_layout_default(&f->ch_layout, layout_requested->nb_channels);
        return 0;
    }
    /* Encoder has a list of supported layouts. Pick the first layout in it with the
       same amount of channels as the requested layout */
    for (i = 0; layouts_allowed[i].nb_channels; i++) {
        if (layouts_allowed[i].nb_channels == layout_requested->nb_channels)
            break;
    }
    if (layouts_allowed[i].nb_channels) {
        /* Use it if one is found */
        err = av_channel_layout_copy(&f->ch_layout, &layouts_allowed[i]);
        if (err < 0)
            return err;
        return 0;
    }
    /* If no layout for the amount of channels requested was found, use the default
       native layout for it. */
    av_channel_layout_default(&f->ch_layout, layout_requested->nb_channels);

    return 0;
}

int ofilter_bind_enc(OutputFilter *ofilter, unsigned sched_idx_enc,
                     const OutputFilterOptions *opts)
{
    OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);
    FilterGraph  *fg = ofilter->graph;
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    int ret;

    av_assert0(!ofilter->bound);
    av_assert0(!opts->enc ||
               ofilter->type == opts->enc->type);

    ofp->needed = ofilter->bound = 1;
    av_freep(&ofilter->linklabel);

    ofp->flags       |= opts->flags;
    ofp->ts_offset    = opts->ts_offset;
    ofp->enc_timebase = opts->output_tb;

    ofp->trim_start_us    = opts->trim_start_us;
    ofp->trim_duration_us = opts->trim_duration_us;

    ofilter->output_name  = av_strdup(opts->name);
    if (!ofilter->output_name)
        return AVERROR(EINVAL);

    ret = av_dict_copy(&ofp->sws_opts, opts->sws_opts, 0);
    if (ret < 0)
        return ret;

    ret = av_dict_copy(&ofp->swr_opts, opts->swr_opts, 0);
    if (ret < 0)
        return ret;

    if (opts->flags & OFILTER_FLAG_AUDIO_24BIT)
        av_dict_set(&ofp->swr_opts, "output_sample_bits", "24", 0);

    if (fgp->is_simple) {
        // for simple filtergraph there is just one output,
        // so use only graph-level information for logging
        ofp->log_parent = NULL;
        av_strlcpy(ofp->log_name, fgp->log_name, sizeof(ofp->log_name));
    } else
        av_strlcatf(ofp->log_name, sizeof(ofp->log_name), "->%s", ofilter->output_name);

    switch (ofilter->type) {
    case AVMEDIA_TYPE_VIDEO:
        ofp->width      = opts->width;
        ofp->height     = opts->height;
        if (opts->format != AV_PIX_FMT_NONE) {
            ofp->format = opts->format;
        } else
            ofp->pix_fmts = opts->pix_fmts;

        if (opts->color_space != AVCOL_SPC_UNSPECIFIED)
            ofp->color_space = opts->color_space;
        else
            ofp->color_spaces = opts->color_spaces;

        if (opts->color_range != AVCOL_RANGE_UNSPECIFIED)
            ofp->color_range = opts->color_range;
        else
            ofp->color_ranges = opts->color_ranges;

        if (opts->alpha_mode != AVALPHA_MODE_UNSPECIFIED)
            ofp->alpha_mode = opts->alpha_mode;
        else
            ofp->alpha_modes = opts->alpha_modes;

        fgp->disable_conversions |= !!(ofp->flags & OFILTER_FLAG_DISABLE_CONVERT);

        ofp->fps.last_frame = av_frame_alloc();
        if (!ofp->fps.last_frame)
            return AVERROR(ENOMEM);

        ofp->fps.vsync_method        = opts->vsync_method;
        ofp->fps.framerate           = opts->frame_rate;
        ofp->fps.framerate_max       = opts->max_frame_rate;
        ofp->fps.framerate_supported = opts->frame_rates;

        // reduce frame rate for mpeg4 to be within the spec limits
        if (opts->enc && opts->enc->id == AV_CODEC_ID_MPEG4)
            ofp->fps.framerate_clip = 65535;

        ofp->fps.dup_warning         = 1000;

        break;
    case AVMEDIA_TYPE_AUDIO:
        if (opts->format != AV_SAMPLE_FMT_NONE) {
            ofp->format = opts->format;
        } else {
            ofp->sample_fmts = opts->sample_fmts;
        }
        if (opts->sample_rate) {
            ofp->sample_rate = opts->sample_rate;
        } else
            ofp->sample_rates = opts->sample_rates;
        if (opts->ch_layout.nb_channels) {
            int ret = set_channel_layout(ofp, opts->ch_layouts, &opts->ch_layout);
            if (ret < 0)
                return ret;
        } else {
            ofp->ch_layouts = opts->ch_layouts;
        }
        break;
    }

    ret = sch_connect(fgp->sch, SCH_FILTER_OUT(fgp->sch_idx, ofilter->index),
                                SCH_ENC(sched_idx_enc));
    if (ret < 0)
        return ret;

    return 0;
}

static int ofilter_bind_ifilter(OutputFilter *ofilter, InputFilterPriv *ifp,
                                const OutputFilterOptions *opts)
{
    OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);

    av_assert0(!ofilter->bound);
    av_assert0(ofilter->type == ifp->ifilter.type);

    ofp->needed = ofilter->bound = 1;
    av_freep(&ofilter->linklabel);

    ofilter->output_name = av_strdup(opts->name);
    if (!ofilter->output_name)
        return AVERROR(EINVAL);

    ifp->ofilter_src = ofilter;

    av_strlcatf(ofp->log_name, sizeof(ofp->log_name), "->%s", ofilter->output_name);

    return 0;
}

static int ifilter_bind_fg(InputFilterPriv *ifp, FilterGraph *fg_src, int out_idx)
{
    FilterGraphPriv      *fgp = fgp_from_fg(ifp->ifilter.graph);
    OutputFilter *ofilter_src = fg_src->outputs[out_idx];
    OutputFilterOptions opts;
    char name[32];
    int ret;

    av_assert0(!ifp->bound);
    ifp->bound = 1;

    if (ifp->ifilter.type != ofilter_src->type) {
        av_log(fgp, AV_LOG_ERROR, "Tried to connect %s output to %s input\n",
               av_get_media_type_string(ofilter_src->type),
               av_get_media_type_string(ifp->ifilter.type));
        return AVERROR(EINVAL);
    }

    ifp->type_src = ifp->ifilter.type;

    memset(&opts, 0, sizeof(opts));

    snprintf(name, sizeof(name), "fg:%d:%d", fgp->fg.index, ifp->ifilter.index);
    opts.name = name;

    ret = ofilter_bind_ifilter(ofilter_src, ifp, &opts);
    if (ret < 0)
        return ret;

    ret = sch_connect(fgp->sch, SCH_FILTER_OUT(fg_src->index, out_idx),
                                SCH_FILTER_IN(fgp->sch_idx, ifp->ifilter.index));
    if (ret < 0)
        return ret;

    return 0;
}

static InputFilter *ifilter_alloc(FilterGraph *fg)
{
    InputFilterPriv *ifp;
    InputFilter *ifilter;

    ifp = allocate_array_elem(&fg->inputs, sizeof(*ifp), &fg->nb_inputs);
    if (!ifp)
        return NULL;

    ifilter         = &ifp->ifilter;
    ifilter->graph  = fg;

    ifp->frame = av_frame_alloc();
    if (!ifp->frame)
        return NULL;

    ifilter->index       = fg->nb_inputs - 1;
    ifp->format          = -1;
    ifp->color_space     = AVCOL_SPC_UNSPECIFIED;
    ifp->color_range     = AVCOL_RANGE_UNSPECIFIED;
    ifp->alpha_mode      = AVALPHA_MODE_UNSPECIFIED;

    ifp->frame_queue = av_fifo_alloc2(8, sizeof(AVFrame*), AV_FIFO_FLAG_AUTO_GROW);
    if (!ifp->frame_queue)
        return NULL;

    return ifilter;
}

void fg_free(FilterGraph **pfg)
{
    FilterGraph *fg = *pfg;
    FilterGraphPriv *fgp;

    if (!fg)
        return;
    fgp = fgp_from_fg(fg);

    for (int j = 0; j < fg->nb_inputs; j++) {
        InputFilter *ifilter = fg->inputs[j];
        InputFilterPriv *ifp = ifp_from_ifilter(ifilter);

        if (ifp->frame_queue) {
            AVFrame *frame;
            while (av_fifo_read(ifp->frame_queue, &frame, 1) >= 0)
                av_frame_free(&frame);
            av_fifo_freep2(&ifp->frame_queue);
        }
        av_frame_free(&ifp->sub2video.frame);

        av_frame_free(&ifp->frame);
        av_frame_free(&ifp->opts.fallback);

        av_buffer_unref(&ifp->hw_frames_ctx);
        av_freep(&ifilter->linklabel);
        av_freep(&ifp->opts.name);
        av_frame_side_data_free(&ifp->side_data, &ifp->nb_side_data);
        av_freep(&ifilter->name);
        av_freep(&ifilter->input_name);
        av_freep(&fg->inputs[j]);
    }
    av_freep(&fg->inputs);
    for (int j = 0; j < fg->nb_outputs; j++) {
        OutputFilter *ofilter = fg->outputs[j];
        OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);

        av_frame_free(&ofp->fps.last_frame);
        av_dict_free(&ofp->sws_opts);
        av_dict_free(&ofp->swr_opts);

        av_freep(&ofilter->linklabel);
        av_freep(&ofilter->name);
        av_freep(&ofilter->output_name);
        av_freep(&ofilter->apad);
        av_channel_layout_uninit(&ofp->ch_layout);
        av_frame_side_data_free(&ofp->side_data, &ofp->nb_side_data);
        av_freep(&fg->outputs[j]);
    }
    av_freep(&fg->outputs);
    av_freep(&fg->graph_desc);

    av_frame_free(&fgp->frame);
    av_frame_free(&fgp->frame_enc);

    av_freep(pfg);
}

static const char *fg_item_name(void *obj)
{
    const FilterGraphPriv *fgp = obj;

    return fgp->log_name;
}

static const AVClass fg_class = {
    .class_name = "FilterGraph",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = fg_item_name,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

int fg_create(FilterGraph **pfg, char **graph_desc, Scheduler *sch,
              const OutputFilterOptions *opts)
{
    FilterGraphPriv *fgp;
    FilterGraph      *fg;

    AVFilterInOut *inputs, *outputs;
    AVFilterGraph *graph;
    int ret = 0;

    fgp = av_mallocz(sizeof(*fgp));
    if (!fgp) {
        av_freep(graph_desc);
        return AVERROR(ENOMEM);
    }
    fg = &fgp->fg;

    if (pfg) {
        *pfg = fg;
        fg->index = -1;
    } else {
        ret = av_dynarray_add_nofree(&filtergraphs, &nb_filtergraphs, fgp);
        if (ret < 0) {
            av_freep(graph_desc);
            av_freep(&fgp);
            return ret;
        }

        fg->index = nb_filtergraphs - 1;
    }

    fg->class       = &fg_class;
    fg->graph_desc  = *graph_desc;
    fgp->disable_conversions = !auto_conversion_filters;
    fgp->nb_threads          = -1;
    fgp->sch                 = sch;

    *graph_desc = NULL;

    snprintf(fgp->log_name, sizeof(fgp->log_name), "fc#%d", fg->index);

    fgp->frame     = av_frame_alloc();
    fgp->frame_enc = av_frame_alloc();
    if (!fgp->frame || !fgp->frame_enc)
        return AVERROR(ENOMEM);

    /* this graph is only used for determining the kinds of inputs
     * and outputs we have, and is discarded on exit from this function */
    graph = avfilter_graph_alloc();
    if (!graph)
        return AVERROR(ENOMEM);;
    graph->nb_threads = 1;

    ret = graph_parse(fg, graph, fg->graph_desc, &inputs, &outputs,
                      hw_device_for_filter());
    if (ret < 0)
        goto fail;

    for (unsigned i = 0; i < graph->nb_filters; i++) {
        const AVFilter *f = graph->filters[i]->filter;
        if ((!avfilter_filter_pad_count(f, 0) &&
             !(f->flags & AVFILTER_FLAG_DYNAMIC_INPUTS)) ||
            !strcmp(f->name, "apad")) {
            fgp->have_sources = 1;
            break;
        }
    }

    for (AVFilterInOut *cur = inputs; cur; cur = cur->next) {
        InputFilter *const ifilter = ifilter_alloc(fg);

        if (!ifilter) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ifilter->linklabel = cur->name;
        cur->name      = NULL;

        ifilter->type  = avfilter_pad_get_type(cur->filter_ctx->input_pads,
                                               cur->pad_idx);

        if (ifilter->type != AVMEDIA_TYPE_VIDEO && ifilter->type != AVMEDIA_TYPE_AUDIO) {
            av_log(fg, AV_LOG_FATAL, "Only video and audio filters supported "
                   "currently.\n");
            ret = AVERROR(ENOSYS);
            goto fail;
        }

        ifilter->name  = describe_filter_link(fg, cur, 1);
        if (!ifilter->name) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    for (AVFilterInOut *cur = outputs; cur; cur = cur->next) {
        const enum AVMediaType type = avfilter_pad_get_type(cur->filter_ctx->output_pads,
                                                            cur->pad_idx);
        OutputFilter *const ofilter = ofilter_alloc(fg, type);
        OutputFilterPriv *ofp;

        if (!ofilter) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ofp = ofp_from_ofilter(ofilter);

        ofilter->linklabel = cur->name;
        cur->name          = NULL;

        ofilter->name      = describe_filter_link(fg, cur, 0);
        if (!ofilter->name) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        // opts should only be needed in this function to fill fields from filtergraphs
        // whose output is meant to be treated as if it was stream, e.g. merged HEIF
        // tile groups.
        if (opts) {
            ofp->flags        = opts->flags;
            ofp->side_data    = opts->side_data;
            ofp->nb_side_data = opts->nb_side_data;

            ofp->crop_top     = opts->crop_top;
            ofp->crop_bottom  = opts->crop_bottom;
            ofp->crop_left    = opts->crop_left;
            ofp->crop_right   = opts->crop_right;

            const AVFrameSideData *sd = av_frame_side_data_get(ofp->side_data, ofp->nb_side_data,
                                                               AV_FRAME_DATA_DISPLAYMATRIX);
            if (sd)
                memcpy(ofp->displaymatrix, sd->data, sizeof(ofp->displaymatrix));
        }
    }

    if (!fg->nb_outputs) {
        av_log(fg, AV_LOG_FATAL, "A filtergraph has zero outputs, this is not supported\n");
        ret = AVERROR(ENOSYS);
        goto fail;
    }

    ret = sch_add_filtergraph(sch, fg->nb_inputs, fg->nb_outputs,
                              filter_thread, fgp);
    if (ret < 0)
        goto fail;
    fgp->sch_idx = ret;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);

    if (ret < 0)
        return ret;

    return 0;
}

int fg_create_simple(FilterGraph **pfg,
                     InputStream *ist,
                     char **graph_desc,
                     Scheduler *sch, unsigned sched_idx_enc,
                     const OutputFilterOptions *opts)
{
    const enum AVMediaType type = ist->par->codec_type;
    FilterGraph *fg;
    FilterGraphPriv *fgp;
    int ret;

    ret = fg_create(pfg, graph_desc, sch, NULL);
    if (ret < 0)
        return ret;
    fg  = *pfg;
    fgp = fgp_from_fg(fg);

    fgp->is_simple = 1;

    snprintf(fgp->log_name, sizeof(fgp->log_name), "%cf%s",
             av_get_media_type_string(type)[0], opts->name);

    if (fg->nb_inputs != 1 || fg->nb_outputs != 1) {
        av_log(fg, AV_LOG_ERROR, "Simple filtergraph '%s' was expected "
               "to have exactly 1 input and 1 output. "
               "However, it had %d input(s) and %d output(s). Please adjust, "
               "or use a complex filtergraph (-filter_complex) instead.\n",
               *graph_desc, fg->nb_inputs, fg->nb_outputs);
        return AVERROR(EINVAL);
    }
    if (fg->outputs[0]->type != type) {
        av_log(fg, AV_LOG_ERROR, "Filtergraph has a %s output, cannot connect "
               "it to %s output stream\n",
               av_get_media_type_string(fg->outputs[0]->type),
               av_get_media_type_string(type));
        return AVERROR(EINVAL);
    }

    ret = ifilter_bind_ist(fg->inputs[0], ist, opts->vs);
    if (ret < 0)
        return ret;

    ret = ofilter_bind_enc(fg->outputs[0], sched_idx_enc, opts);
    if (ret < 0)
        return ret;

    if (opts->nb_threads >= 0)
        fgp->nb_threads = opts->nb_threads;

    return 0;
}

static int fg_complex_bind_input(FilterGraph *fg, InputFilter *ifilter, int commit)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    InputStream *ist = NULL;
    enum AVMediaType type = ifilter->type;
    ViewSpecifier vs = { .type = VIEW_SPECIFIER_TYPE_NONE };
    const char *spec;
    char *p;
    int i, ret;

    if (ifilter->linklabel && !strncmp(ifilter->linklabel, "dec:", 4)) {
        // bind to a standalone decoder
        int dec_idx;

        dec_idx = strtol(ifilter->linklabel + 4, &p, 0);
        if (dec_idx < 0 || dec_idx >= nb_decoders) {
            av_log(fg, AV_LOG_ERROR, "Invalid decoder index %d in filtergraph description %s\n",
                   dec_idx, fg->graph_desc);
            return AVERROR(EINVAL);
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            spec = *p == ':' ? p + 1 : p;
            ret = view_specifier_parse(&spec, &vs);
            if (ret < 0)
                return ret;
        }

        ret = ifilter_bind_dec(ifp, decoders[dec_idx], &vs);
        if (ret < 0)
            av_log(fg, AV_LOG_ERROR, "Error binding a decoder to filtergraph input %s\n",
                   ifilter->name);
        return ret;
    } else if (ifilter->linklabel) {
        StreamSpecifier ss;
        AVFormatContext *s;
        AVStream       *st = NULL;
        int file_idx;

        // try finding an unbound filtergraph output with this label
        for (int i = 0; i < nb_filtergraphs; i++) {
            FilterGraph *fg_src = filtergraphs[i];

            if (fg == fg_src)
                continue;

            for (int j = 0; j < fg_src->nb_outputs; j++) {
                OutputFilter *ofilter = fg_src->outputs[j];

                if (!ofilter->bound && ofilter->linklabel &&
                    !strcmp(ofilter->linklabel, ifilter->linklabel)) {
                    if (commit) {
                        av_log(fg, AV_LOG_VERBOSE,
                               "Binding input with label '%s' to filtergraph output %d:%d\n",
                               ifilter->linklabel, i, j);

                        ret = ifilter_bind_fg(ifp, fg_src, j);
                        if (ret < 0) {
                            av_log(fg, AV_LOG_ERROR, "Error binding filtergraph input %s\n",
                                   ifilter->linklabel);
                            return ret;
                        }
                    } else
                        ofp_from_ofilter(ofilter)->needed = 1;
                    return 0;
                }
            }
        }

        // bind to an explicitly specified demuxer stream
        file_idx = strtol(ifilter->linklabel, &p, 0);
        if (file_idx < 0 || file_idx >= nb_input_files) {
            av_log(fg, AV_LOG_FATAL, "Invalid file index %d in filtergraph description %s.\n",
                   file_idx, fg->graph_desc);
            return AVERROR(EINVAL);
        }
        s = input_files[file_idx]->ctx;

        ret = stream_specifier_parse(&ss, *p == ':' ? p + 1 : p, 1, fg);
        if (ret < 0) {
            av_log(fg, AV_LOG_ERROR, "Invalid stream specifier: %s\n", p);
            return ret;
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            spec = ss.remainder ? ss.remainder : "";
            ret = view_specifier_parse(&spec, &vs);
            if (ret < 0) {
                stream_specifier_uninit(&ss);
                return ret;
            }
        }

        for (i = 0; i < s->nb_streams; i++) {
            enum AVMediaType stream_type = s->streams[i]->codecpar->codec_type;
            if (stream_type != type &&
                !(stream_type == AVMEDIA_TYPE_SUBTITLE &&
                  type == AVMEDIA_TYPE_VIDEO /* sub2video hack */))
                continue;
            if (stream_specifier_match(&ss, s, s->streams[i], fg)) {
                st = s->streams[i];
                break;
            }
        }
        stream_specifier_uninit(&ss);
        if (!st) {
            av_log(fg, AV_LOG_FATAL, "Stream specifier '%s' in filtergraph description %s "
                   "matches no streams.\n", p, fg->graph_desc);
            return AVERROR(EINVAL);
        }
        ist = input_files[file_idx]->streams[st->index];

        if (commit)
            av_log(fg, AV_LOG_VERBOSE,
                   "Binding input with label '%s' to input stream %d:%d\n",
                   ifilter->linklabel, ist->file->index, ist->index);
    } else {
        // try finding an unbound filtergraph output
        for (int i = 0; i < nb_filtergraphs; i++) {
            FilterGraph *fg_src = filtergraphs[i];

            if (fg == fg_src)
                continue;

            for (int j = 0; j < fg_src->nb_outputs; j++) {
                OutputFilter *ofilter = fg_src->outputs[j];

                if (!ofilter->bound) {
                    if (commit) {
                        av_log(fg, AV_LOG_VERBOSE,
                               "Binding unlabeled filtergraph input to filtergraph output %d:%d\n", i, j);

                        ret = ifilter_bind_fg(ifp, fg_src, j);
                        if (ret < 0) {
                            av_log(fg, AV_LOG_ERROR, "Error binding filtergraph input %d:%d\n", i, j);
                            return ret;
                        }
                    } else
                        ofp_from_ofilter(ofilter)->needed = 1;
                    return 0;
                }
            }
        }

        ist = ist_find_unused(type);
        if (!ist) {
            av_log(fg, AV_LOG_FATAL,
                   "Cannot find an unused %s input stream to feed the "
                   "unlabeled input pad %s.\n",
                   av_get_media_type_string(type), ifilter->name);
            return AVERROR(EINVAL);
        }

        if (commit)
            av_log(fg, AV_LOG_VERBOSE,
                   "Binding unlabeled input %d to input stream %d:%d\n",
                   ifilter->index, ist->file->index, ist->index);
    }
    av_assert0(ist);

    if (commit) {
        ret = ifilter_bind_ist(ifilter, ist, &vs);
        if (ret < 0) {
            av_log(fg, AV_LOG_ERROR,
                   "Error binding an input stream to complex filtergraph input %s.\n",
                   ifilter->name);
            return ret;
        }
    }

    return 0;
}

static int bind_inputs(FilterGraph *fg, int commit)
{
    // bind filtergraph inputs to input streams or other filtergraphs
    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilterPriv *ifp = ifp_from_ifilter(fg->inputs[i]);
        int ret;

        if (ifp->bound)
            continue;

        ret = fg_complex_bind_input(fg, &ifp->ifilter, commit);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int fg_finalise_bindings(void)
{
    int ret;

    for (int i = 0; i < nb_filtergraphs; i++) {
        ret = bind_inputs(filtergraphs[i], 0);
        if (ret < 0)
            return ret;
    }

    // check that all outputs were bound
    for (int i = nb_filtergraphs - 1; i >= 0; i--) {
        FilterGraph *fg = filtergraphs[i];
        FilterGraphPriv *fgp = fgp_from_fg(filtergraphs[i]);

        for (int j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *output = fg->outputs[j];
            if (!ofp_from_ofilter(output)->needed) {
                if (!fg->is_internal) {
                    av_log(fg, AV_LOG_FATAL,
                           "Filter '%s' has output %d (%s) unconnected\n",
                           output->name, j,
                           output->linklabel ? (const char *)output->linklabel : "unlabeled");
                    return AVERROR(EINVAL);
                }

                av_log(fg, AV_LOG_DEBUG,
                       "Internal filter '%s' has output %d (%s) unconnected. Removing graph\n",
                       output->name, j,
                       output->linklabel ? (const char *)output->linklabel : "unlabeled");
                sch_remove_filtergraph(fgp->sch, fgp->sch_idx);
                fg_free(&filtergraphs[i]);
                nb_filtergraphs--;
                if (nb_filtergraphs > 0)
                    memmove(&filtergraphs[i],
                            &filtergraphs[i + 1],
                            (nb_filtergraphs - i) * sizeof(*filtergraphs));
                break;
            }
        }
    }

    for (int i = 0; i < nb_filtergraphs; i++) {
        ret = bind_inputs(filtergraphs[i], 1);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int insert_trim(void *logctx, int64_t start_time, int64_t duration,
                       AVFilterContext **last_filter, int *pad_idx,
                       const char *filter_name)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    AVFilterContext *ctx;
    const AVFilter *trim;
    enum AVMediaType type = avfilter_pad_get_type((*last_filter)->output_pads, *pad_idx);
    const char *name = (type == AVMEDIA_TYPE_VIDEO) ? "trim" : "atrim";
    int ret = 0;

    if (duration == INT64_MAX && start_time == AV_NOPTS_VALUE)
        return 0;

    trim = avfilter_get_by_name(name);
    if (!trim) {
        av_log(logctx, AV_LOG_ERROR, "%s filter not present, cannot limit "
               "recording time.\n", name);
        return AVERROR_FILTER_NOT_FOUND;
    }

    ctx = avfilter_graph_alloc_filter(graph, trim, filter_name);
    if (!ctx)
        return AVERROR(ENOMEM);

    if (duration != INT64_MAX) {
        ret = av_opt_set_int(ctx, "durationi", duration,
                                AV_OPT_SEARCH_CHILDREN);
    }
    if (ret >= 0 && start_time != AV_NOPTS_VALUE) {
        ret = av_opt_set_int(ctx, "starti", start_time,
                                AV_OPT_SEARCH_CHILDREN);
    }
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error configuring the %s filter", name);
        return ret;
    }

    ret = avfilter_init_str(ctx, NULL);
    if (ret < 0)
        return ret;

    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}

static int insert_filter(AVFilterContext **last_filter, int *pad_idx,
                         const char *filter_name, const char *args)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    const AVFilter *filter = avfilter_get_by_name(filter_name);
    AVFilterContext *ctx;
    int ret;

    if (!filter)
        return AVERROR_BUG;

    ret = avfilter_graph_create_filter(&ctx,
                                       filter,
                                       filter_name, args, NULL, graph);
    if (ret < 0)
        return ret;

    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}

static int configure_output_video_filter(FilterGraphPriv *fgp, AVFilterGraph *graph,
                                         OutputFilter *ofilter, AVFilterInOut *out)
{
    OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);
    AVFilterContext *last_filter = out->filter_ctx;
    AVBPrint bprint;
    int pad_idx = out->pad_idx;
    int ret;
    char name[255];

    snprintf(name, sizeof(name), "out_%s", ofilter->output_name);
    ret = avfilter_graph_create_filter(&ofilter->filter,
                                       avfilter_get_by_name("buffersink"),
                                       name, NULL, NULL, graph);

    if (ret < 0)
        return ret;

    if (ofp->flags & OFILTER_FLAG_CROP) {
        char crop_buf[64];
        snprintf(crop_buf, sizeof(crop_buf), "w=iw-%u-%u:h=ih-%u-%u:x=%u:y=%u",
                 ofp->crop_left, ofp->crop_right,
                 ofp->crop_top,  ofp->crop_bottom,
                 ofp->crop_left, ofp->crop_top);
        ret = insert_filter(&last_filter, &pad_idx, "crop", crop_buf);
        if (ret < 0)
            return ret;
    }

    if (ofp->flags & OFILTER_FLAG_AUTOROTATE) {
        int32_t *displaymatrix = ofp->displaymatrix;
        double theta;

        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose",
                                displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "hflip", NULL);
                if (ret < 0)
                    return ret;
            }
            if (displaymatrix[4] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);
            }
        } else if (fabs(theta - 270) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose",
                                displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            ret = insert_filter(&last_filter, &pad_idx, "rotate", rotate_buf);
        } else if (fabs(theta) < 1.0) {
            if (displaymatrix && displaymatrix[4] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);
            }
        }
        if (ret < 0)
            return ret;

        av_frame_side_data_remove(&ofp->side_data, &ofp->nb_side_data, AV_FRAME_DATA_DISPLAYMATRIX);
    }

    if ((ofp->width || ofp->height) && (ofp->flags & OFILTER_FLAG_AUTOSCALE)) {
        char args[255];
        AVFilterContext *filter;
        const AVDictionaryEntry *e = NULL;

        snprintf(args, sizeof(args), "%d:%d",
                 ofp->width, ofp->height);

        while ((e = av_dict_iterate(ofp->sws_opts, e))) {
            av_strlcatf(args, sizeof(args), ":%s=%s", e->key, e->value);
        }

        snprintf(name, sizeof(name), "scaler_out_%s", ofilter->output_name);
        if ((ret = avfilter_graph_create_filter(&filter, avfilter_get_by_name("scale"),
                                                name, args, NULL, graph)) < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx = 0;
    }

    av_assert0(!(ofp->flags & OFILTER_FLAG_DISABLE_CONVERT) ||
               ofp->format != AV_PIX_FMT_NONE || !ofp->pix_fmts);
    av_bprint_init(&bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
    choose_pix_fmts(ofp, &bprint);
    choose_color_spaces(ofp, &bprint);
    choose_color_ranges(ofp, &bprint);
    choose_alpha_modes(ofp, &bprint);
    if (!av_bprint_is_complete(&bprint))
        return AVERROR(ENOMEM);

    if (bprint.len) {
        AVFilterContext *filter;

        ret = avfilter_graph_create_filter(&filter,
                                           avfilter_get_by_name("format"),
                                           "format", bprint.str, NULL, graph);
        av_bprint_finalize(&bprint, NULL);
        if (ret < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx     = 0;
    }

    snprintf(name, sizeof(name), "trim_out_%s", ofilter->output_name);
    ret = insert_trim(fgp, ofp->trim_start_us, ofp->trim_duration_us,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;


    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        return ret;

    return 0;
}

static int configure_output_audio_filter(FilterGraphPriv *fgp, AVFilterGraph *graph,
                                         OutputFilter *ofilter, AVFilterInOut *out)
{
    OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);
    AVFilterContext *last_filter = out->filter_ctx;
    int pad_idx = out->pad_idx;
    AVBPrint args;
    char name[255];
    int ret;

    snprintf(name, sizeof(name), "out_%s", ofilter->output_name);
    ret = avfilter_graph_create_filter(&ofilter->filter,
                                       avfilter_get_by_name("abuffersink"),
                                       name, NULL, NULL, graph);
    if (ret < 0)
        return ret;

#define AUTO_INSERT_FILTER(opt_name, filter_name, arg) do {                 \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    av_log(ofilter, AV_LOG_INFO, opt_name " is forwarded to lavfi "         \
           "similarly to -af " filter_name "=%s.\n", arg);                  \
                                                                            \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(filter_name),   \
                                       filter_name, arg, NULL, graph);      \
    if (ret < 0)                                                            \
        goto fail;                                                          \
                                                                            \
    ret = avfilter_link(last_filter, pad_idx, filt_ctx, 0);                 \
    if (ret < 0)                                                            \
        goto fail;                                                          \
                                                                            \
    last_filter = filt_ctx;                                                 \
    pad_idx = 0;                                                            \
} while (0)
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_UNLIMITED);

    choose_sample_fmts(ofp,     &args);
    choose_sample_rates(ofp,    &args);
    choose_channel_layouts(ofp, &args);
    if (!av_bprint_is_complete(&args)) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    if (args.len) {
        AVFilterContext *format;

        snprintf(name, sizeof(name), "format_out_%s", ofilter->output_name);
        ret = avfilter_graph_create_filter(&format,
                                           avfilter_get_by_name("aformat"),
                                           name, args.str, NULL, graph);
        if (ret < 0)
            goto fail;

        ret = avfilter_link(last_filter, pad_idx, format, 0);
        if (ret < 0)
            goto fail;

        last_filter = format;
        pad_idx = 0;
    }

    if (ofilter->apad) {
        AUTO_INSERT_FILTER("-apad", "apad", ofilter->apad);
        fgp->have_sources = 1;
    }

    snprintf(name, sizeof(name), "trim for output %s", ofilter->output_name);
    ret = insert_trim(fgp, ofp->trim_start_us, ofp->trim_duration_us,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        goto fail;

    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        goto fail;
fail:
    av_bprint_finalize(&args, NULL);

    return ret;
}

static int configure_output_filter(FilterGraphPriv *fgp, AVFilterGraph *graph,
                                   OutputFilter *ofilter, AVFilterInOut *out)
{
    switch (ofilter->type) {
    case AVMEDIA_TYPE_VIDEO: return configure_output_video_filter(fgp, graph, ofilter, out);
    case AVMEDIA_TYPE_AUDIO: return configure_output_audio_filter(fgp, graph, ofilter, out);
    default: av_assert0(0); return 0;
    }
}

static void sub2video_prepare(InputFilterPriv *ifp)
{
    ifp->sub2video.last_pts = INT64_MIN;
    ifp->sub2video.end_pts  = INT64_MIN;

    /* sub2video structure has been (re-)initialized.
       Mark it as such so that the system will be
       initialized with the first received heartbeat. */
    ifp->sub2video.initialize = 1;
}

static int configure_input_video_filter(FilterGraph *fg, AVFilterGraph *graph,
                                        InputFilter *ifilter, AVFilterInOut *in)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);

    AVFilterContext *last_filter;
    const AVFilter *buffer_filt = avfilter_get_by_name("buffer");
    const AVPixFmtDescriptor *desc;
    char name[255];
    int ret, pad_idx = 0;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);

    if (ifp->type_src == AVMEDIA_TYPE_SUBTITLE)
        sub2video_prepare(ifp);

    snprintf(name, sizeof(name), "graph %d input from stream %s", fg->index,
             ifp->opts.name);

    ifilter->filter = avfilter_graph_alloc_filter(graph, buffer_filt, name);
    if (!ifilter->filter) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = ifp->format;
    par->time_base           = ifp->time_base;
    par->frame_rate          = ifp->opts.framerate;
    par->width               = ifp->width;
    par->height              = ifp->height;
    par->sample_aspect_ratio = ifp->sample_aspect_ratio.den > 0 ?
                               ifp->sample_aspect_ratio : (AVRational){ 0, 1 };
    par->color_space         = ifp->color_space;
    par->color_range         = ifp->color_range;
    par->alpha_mode          = ifp->alpha_mode;
    par->hw_frames_ctx       = ifp->hw_frames_ctx;
    par->side_data           = ifp->side_data;
    par->nb_side_data        = ifp->nb_side_data;

    ret = av_buffersrc_parameters_set(ifilter->filter, par);
    if (ret < 0)
        goto fail;
    av_freep(&par);

    ret = avfilter_init_dict(ifilter->filter, NULL);
    if (ret < 0)
        goto fail;

    last_filter = ifilter->filter;

    desc = av_pix_fmt_desc_get(ifp->format);
    av_assert0(desc);

    if ((ifp->opts.flags & IFILTER_FLAG_CROP)) {
        char crop_buf[64];
        snprintf(crop_buf, sizeof(crop_buf), "w=iw-%u-%u:h=ih-%u-%u:x=%u:y=%u",
                 ifp->opts.crop_left, ifp->opts.crop_right,
                 ifp->opts.crop_top, ifp->opts.crop_bottom,
                 ifp->opts.crop_left, ifp->opts.crop_top);
        ret = insert_filter(&last_filter, &pad_idx, "crop", crop_buf);
        if (ret < 0)
            return ret;
    }

    // TODO: insert hwaccel enabled filters like transpose_vaapi into the graph
    ifp->displaymatrix_applied = 0;
    if ((ifp->opts.flags & IFILTER_FLAG_AUTOROTATE) &&
        !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        int32_t *displaymatrix = ifp->displaymatrix;
        double theta;

        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose",
                                displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "hflip", NULL);
                if (ret < 0)
                    return ret;
            }
            if (displaymatrix[4] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);
            }
        } else if (fabs(theta - 270) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose",
                                displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            ret = insert_filter(&last_filter, &pad_idx, "rotate", rotate_buf);
        } else if (fabs(theta) < 1.0) {
            if (displaymatrix && displaymatrix[4] < 0) {
                ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);
            }
        }
        if (ret < 0)
            return ret;

        ifp->displaymatrix_applied = 1;
    }

    snprintf(name, sizeof(name), "trim_in_%s", ifp->opts.name);
    ret = insert_trim(fg, ifp->opts.trim_start_us, ifp->opts.trim_end_us,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)
        return ret;
    return 0;
fail:
    av_freep(&par);

    return ret;
}

static int configure_input_audio_filter(FilterGraph *fg, AVFilterGraph *graph,
                                        InputFilter *ifilter, AVFilterInOut *in)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    AVFilterContext *last_filter;
    AVBufferSrcParameters *par;
    const AVFilter *abuffer_filt = avfilter_get_by_name("abuffer");
    AVBPrint args;
    char name[255];
    int ret, pad_idx = 0;

    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
               ifp->time_base.num, ifp->time_base.den,
               ifp->sample_rate,
               av_get_sample_fmt_name(ifp->format));
    if (av_channel_layout_check(&ifp->ch_layout) &&
        ifp->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_bprintf(&args, ":channel_layout=");
        av_channel_layout_describe_bprint(&ifp->ch_layout, &args);
    } else
        av_bprintf(&args, ":channels=%d", ifp->ch_layout.nb_channels);
    snprintf(name, sizeof(name), "graph_%d_in_%s", fg->index, ifp->opts.name);

    if ((ret = avfilter_graph_create_filter(&ifilter->filter, abuffer_filt,
                                            name, args.str, NULL,
                                            graph)) < 0)
        return ret;
    par = av_buffersrc_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);
    par->side_data     = ifp->side_data;
    par->nb_side_data  = ifp->nb_side_data;
    ret = av_buffersrc_parameters_set(ifilter->filter, par);
    av_free(par);
    if (ret < 0)
        return ret;
    last_filter = ifilter->filter;

    snprintf(name, sizeof(name), "trim for input stream %s", ifp->opts.name);
    ret = insert_trim(fg, ifp->opts.trim_start_us, ifp->opts.trim_end_us,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)
        return ret;

    return 0;
}

static int configure_input_filter(FilterGraph *fg, AVFilterGraph *graph,
                                  InputFilter *ifilter, AVFilterInOut *in)
{
    switch (ifilter->type) {
    case AVMEDIA_TYPE_VIDEO: return configure_input_video_filter(fg, graph, ifilter, in);
    case AVMEDIA_TYPE_AUDIO: return configure_input_audio_filter(fg, graph, ifilter, in);
    default: av_assert0(0); return 0;
    }
}

static void cleanup_filtergraph(FilterGraph *fg, FilterGraphThread *fgt)
{
    for (int i = 0; i < fg->nb_outputs; i++)
        fg->outputs[i]->filter = NULL;
    for (int i = 0; i < fg->nb_inputs; i++)
        fg->inputs[i]->filter = NULL;
    avfilter_graph_free(&fgt->graph);
}

static int filter_is_buffersrc(const AVFilterContext *f)
{
    return f->nb_inputs == 0 &&
           (!strcmp(f->filter->name, "buffer") ||
            !strcmp(f->filter->name, "abuffer"));
}

static int graph_is_meta(AVFilterGraph *graph)
{
    for (unsigned i = 0; i < graph->nb_filters; i++) {
        const AVFilterContext *f = graph->filters[i];

        /* in addition to filters flagged as meta, also
         * disregard sinks and buffersources (but not other sources,
         * since they introduce data we are not aware of)
         */
        if (!((f->filter->flags & AVFILTER_FLAG_METADATA_ONLY) ||
              f->nb_outputs == 0                               ||
              filter_is_buffersrc(f)))
            return 0;
    }
    return 1;
}

static int sub2video_frame(InputFilter *ifilter, AVFrame *frame, int buffer);

static int configure_filtergraph(FilterGraph *fg, FilterGraphThread *fgt)
{
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    AVBufferRef *hw_device;
    AVFilterInOut *inputs, *outputs, *cur;
    int ret = AVERROR_BUG, i, simple = filtergraph_is_simple(fg);
    int have_input_eof = 0;
    const char *graph_desc = fg->graph_desc;

    cleanup_filtergraph(fg, fgt);
    fgt->graph = avfilter_graph_alloc();
    if (!fgt->graph)
        return AVERROR(ENOMEM);

    if (simple) {
        OutputFilterPriv *ofp = ofp_from_ofilter(fg->outputs[0]);

        if (filter_nbthreads) {
            ret = av_opt_set(fgt->graph, "threads", filter_nbthreads, 0);
            if (ret < 0)
                goto fail;
        } else if (fgp->nb_threads >= 0) {
            ret = av_opt_set_int(fgt->graph, "threads", fgp->nb_threads, 0);
            if (ret < 0)
                return ret;
        }

        if (av_dict_count(ofp->sws_opts)) {
            ret = av_dict_get_string(ofp->sws_opts,
                                     &fgt->graph->scale_sws_opts,
                                     '=', ':');
            if (ret < 0)
                goto fail;
        }

        if (av_dict_count(ofp->swr_opts)) {
            char *args;
            ret = av_dict_get_string(ofp->swr_opts, &args, '=', ':');
            if (ret < 0)
                goto fail;
            av_opt_set(fgt->graph, "aresample_swr_opts", args, 0);
            av_free(args);
        }
    } else {
        fgt->graph->nb_threads = filter_complex_nbthreads;
    }

    if (filter_buffered_frames) {
        ret = av_opt_set_int(fgt->graph, "max_buffered_frames", filter_buffered_frames, 0);
        if (ret < 0)
            return ret;
    }

    hw_device = hw_device_for_filter();

    ret = graph_parse(fg, fgt->graph, graph_desc, &inputs, &outputs, hw_device);
    if (ret < 0)
        goto fail;

    for (cur = inputs, i = 0; cur; cur = cur->next, i++)
        if ((ret = configure_input_filter(fg, fgt->graph, fg->inputs[i], cur)) < 0) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            goto fail;
        }
    avfilter_inout_free(&inputs);

    for (cur = outputs, i = 0; cur; cur = cur->next, i++) {
        ret = configure_output_filter(fgp, fgt->graph, fg->outputs[i], cur);
        if (ret < 0) {
            avfilter_inout_free(&outputs);
            goto fail;
        }
    }
    avfilter_inout_free(&outputs);

    if (fgp->disable_conversions)
        avfilter_graph_set_auto_convert(fgt->graph, AVFILTER_AUTO_CONVERT_NONE);
    if ((ret = avfilter_graph_config(fgt->graph, NULL)) < 0)
        goto fail;

    fgp->is_meta = graph_is_meta(fgt->graph);

    /* limit the lists of allowed formats to the ones selected, to
     * make sure they stay the same if the filtergraph is reconfigured later */
    for (int i = 0; i < fg->nb_outputs; i++) {
        const AVFrameSideData *const *sd;
        int nb_sd;
        OutputFilter *ofilter = fg->outputs[i];
        OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);
        AVFilterContext *sink = ofilter->filter;

        ofp->format = av_buffersink_get_format(sink);

        ofp->width  = av_buffersink_get_w(sink);
        ofp->height = av_buffersink_get_h(sink);
        ofp->color_space = av_buffersink_get_colorspace(sink);
        ofp->color_range = av_buffersink_get_color_range(sink);
        ofp->alpha_mode = av_buffersink_get_alpha_mode(sink);

        // If the timing parameters are not locked yet, get the tentative values
        // here but don't lock them. They will only be used if no output frames
        // are ever produced.
        if (!ofp->tb_out_locked) {
            AVRational fr = av_buffersink_get_frame_rate(sink);
            if (ofp->fps.framerate.num <= 0 && ofp->fps.framerate.den <= 0 &&
                fr.num > 0 && fr.den > 0)
                ofp->fps.framerate = fr;
            ofp->tb_out = av_buffersink_get_time_base(sink);
        }
        ofp->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);

        ofp->sample_rate    = av_buffersink_get_sample_rate(sink);
        av_channel_layout_uninit(&ofp->ch_layout);
        ret = av_buffersink_get_ch_layout(sink, &ofp->ch_layout);
        if (ret < 0)
            goto fail;
        sd = av_buffersink_get_side_data(sink, &nb_sd);
        if (nb_sd)
            for (int j = 0; j < nb_sd; j++) {
                ret = av_frame_side_data_clone(&ofp->side_data, &ofp->nb_side_data,
                                               sd[j], AV_FRAME_SIDE_DATA_FLAG_REPLACE);
                if (ret < 0) {
                    av_frame_side_data_free(&ofp->side_data, &ofp->nb_side_data);
                    goto fail;
                }
            }
    }

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilter *ifilter = fg->inputs[i];
        InputFilterPriv *ifp = ifp_from_ifilter(fg->inputs[i]);
        AVFrame *tmp;
        while (av_fifo_read(ifp->frame_queue, &tmp, 1) >= 0) {
            if (ifp->type_src == AVMEDIA_TYPE_SUBTITLE) {
                sub2video_frame(&ifp->ifilter, tmp, !fgt->graph);
            } else {
                if (ifp->type_src == AVMEDIA_TYPE_VIDEO) {
                    if (ifp->displaymatrix_applied)
                        av_frame_remove_side_data(tmp, AV_FRAME_DATA_DISPLAYMATRIX);
                }
                ret = av_buffersrc_add_frame(ifilter->filter, tmp);
            }
            av_frame_free(&tmp);
            if (ret < 0)
                goto fail;
        }
    }

    /* send the EOFs for the finished inputs */
    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilter *ifilter = fg->inputs[i];
        if (fgt->eof_in[i]) {
            ret = av_buffersrc_add_frame(ifilter->filter, NULL);
            if (ret < 0)
                goto fail;
            have_input_eof = 1;
        }
    }

    if (have_input_eof) {
        // make sure the EOF propagates to the end of the graph
        ret = avfilter_graph_request_oldest(fgt->graph);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            goto fail;
    }

    return 0;
fail:
    cleanup_filtergraph(fg, fgt);
    return ret;
}

static int ifilter_parameters_from_frame(InputFilter *ifilter, const AVFrame *frame)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    AVFrameSideData *sd;
    int ret;

    ret = av_buffer_replace(&ifp->hw_frames_ctx, frame->hw_frames_ctx);
    if (ret < 0)
        return ret;

    ifp->time_base = (ifilter->type == AVMEDIA_TYPE_AUDIO)    ? (AVRational){ 1, frame->sample_rate } :
                     (ifp->opts.flags & IFILTER_FLAG_CFR) ? av_inv_q(ifp->opts.framerate)         :
                     frame->time_base;

    ifp->format              = frame->format;

    ifp->width               = frame->width;
    ifp->height              = frame->height;
    ifp->sample_aspect_ratio = frame->sample_aspect_ratio;
    ifp->color_space         = frame->colorspace;
    ifp->color_range         = frame->color_range;
    ifp->alpha_mode          = frame->alpha_mode;

    ifp->sample_rate         = frame->sample_rate;
    ret = av_channel_layout_copy(&ifp->ch_layout, &frame->ch_layout);
    if (ret < 0)
        return ret;

    av_frame_side_data_free(&ifp->side_data, &ifp->nb_side_data);
    for (int i = 0; i < frame->nb_side_data; i++) {
        const AVSideDataDescriptor *desc = av_frame_side_data_desc(frame->side_data[i]->type);

        if (!(desc->props & AV_SIDE_DATA_PROP_GLOBAL) ||
            frame->side_data[i]->type == AV_FRAME_DATA_DISPLAYMATRIX)
            continue;

        ret = av_frame_side_data_clone(&ifp->side_data,
                                       &ifp->nb_side_data,
                                       frame->side_data[i], 0);
        if (ret < 0)
            return ret;
    }

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (sd)
        memcpy(ifp->displaymatrix, sd->data, sizeof(ifp->displaymatrix));
    ifp->displaymatrix_present = !!sd;

    /* Copy downmix related side data to InputFilterPriv so it may be propagated
     * to the filter chain even though it's not "global", as filters like aresample
     * require this information during init and not when remixing a frame */
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOWNMIX_INFO);
    if (sd) {
        ret = av_frame_side_data_clone(&ifp->side_data,
                                       &ifp->nb_side_data, sd, 0);
        if (ret < 0)
            return ret;
        memcpy(&ifp->downmixinfo, sd->data, sizeof(ifp->downmixinfo));
    }
    ifp->downmixinfo_present = !!sd;

    return 0;
}

static int ifilter_parameters_from_ofilter(InputFilter *ifilter, OutputFilter *ofilter)
{
    const OutputFilterPriv *ofp = ofp_from_ofilter(ofilter);
    InputFilterPriv  *ifp = ifp_from_ifilter(ifilter);

    if (!ifp->opts.framerate.num) {
        ifp->opts.framerate = ofp->fps.framerate;
        if (ifp->opts.framerate.num > 0 && ifp->opts.framerate.den > 0)
            ifp->opts.flags |= IFILTER_FLAG_CFR;
    }

    for (int i = 0; i < ofp->nb_side_data; i++) {
        int ret = av_frame_side_data_clone(&ifp->side_data, &ifp->nb_side_data,
                                           ofp->side_data[i], AV_FRAME_SIDE_DATA_FLAG_REPLACE);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int filtergraph_is_simple(const FilterGraph *fg)
{
    const FilterGraphPriv *fgp = cfgp_from_cfg(fg);
    return fgp->is_simple;
}

static void send_command(FilterGraph *fg, AVFilterGraph *graph,
                         double time, const char *target,
                         const char *command, const char *arg, int all_filters)
{
    int ret;

    if (!graph)
        return;

    if (time < 0) {
        char response[4096];
        ret = avfilter_graph_send_command(graph, target, command, arg,
                                          response, sizeof(response),
                                          all_filters ? 0 : AVFILTER_CMD_FLAG_ONE);
        fprintf(stderr, "Command reply for stream %d: ret:%d res:\n%s",
                fg->index, ret, response);
    } else if (!all_filters) {
        fprintf(stderr, "Queuing commands only on filters supporting the specific command is unsupported\n");
    } else {
        ret = avfilter_graph_queue_command(graph, target, command, arg, 0, time);
        if (ret < 0)
            fprintf(stderr, "Queuing command failed with error %s\n", av_err2str(ret));
    }
}

static int choose_input(const FilterGraph *fg, const FilterGraphThread *fgt)
{
    int nb_requests, nb_requests_max = -1;
    int best_input = -1;

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilter *ifilter = fg->inputs[i];

        if (fgt->eof_in[i])
            continue;

        nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
        if (nb_requests > nb_requests_max) {
            nb_requests_max = nb_requests;
            best_input = i;
        }
    }

    av_assert0(best_input >= 0);

    return best_input;
}

static int choose_out_timebase(OutputFilterPriv *ofp, AVFrame *frame)
{
    OutputFilter *ofilter = &ofp->ofilter;
    FPSConvContext   *fps = &ofp->fps;
    AVRational        tb = (AVRational){ 0, 0 };
    AVRational fr;
    const FrameData *fd;

    fd = frame_data_c(frame);

    // apply -enc_time_base
    if (ofp->enc_timebase.num == ENC_TIME_BASE_DEMUX &&
        (fd->dec.tb.num <= 0 || fd->dec.tb.den <= 0)) {
        av_log(ofp, AV_LOG_ERROR,
               "Demuxing timebase not available - cannot use it for encoding\n");
        return AVERROR(EINVAL);
    }

    switch (ofp->enc_timebase.num) {
    case 0:                                            break;
    case ENC_TIME_BASE_DEMUX:  tb = fd->dec.tb;        break;
    case ENC_TIME_BASE_FILTER: tb = frame->time_base;  break;
    default:                   tb = ofp->enc_timebase; break;
    }

    if (ofilter->type == AVMEDIA_TYPE_AUDIO) {
        tb = tb.num ? tb : (AVRational){ 1, frame->sample_rate };
        goto finish;
    }

    fr = fps->framerate;
    if (!fr.num) {
        AVRational fr_sink = av_buffersink_get_frame_rate(ofilter->filter);
        if (fr_sink.num > 0 && fr_sink.den > 0)
            fr = fr_sink;
    }

    if (fps->vsync_method == VSYNC_CFR || fps->vsync_method == VSYNC_VSCFR) {
        if (!fr.num && !fps->framerate_max.num) {
            fr = (AVRational){25, 1};
            av_log(ofp, AV_LOG_WARNING,
                   "No information "
                   "about the input framerate is available. Falling "
                   "back to a default value of 25fps. Use the -r option "
                   "if you want a different framerate.\n");
        }

        if (fps->framerate_max.num &&
            (av_q2d(fr) > av_q2d(fps->framerate_max) ||
            !fr.den))
            fr = fps->framerate_max;
    }

    if (fr.num > 0) {
        if (fps->framerate_supported) {
            int idx = av_find_nearest_q_idx(fr, fps->framerate_supported);
            fr = fps->framerate_supported[idx];
        }
        if (fps->framerate_clip) {
            av_reduce(&fr.num, &fr.den,
                      fr.num, fr.den, fps->framerate_clip);
        }
    }

    if (!(tb.num > 0 && tb.den > 0))
        tb = av_inv_q(fr);
    if (!(tb.num > 0 && tb.den > 0))
        tb = frame->time_base;

    fps->framerate     = fr;
finish:
    ofp->tb_out        = tb;
    ofp->tb_out_locked = 1;

    return 0;
}

static double adjust_frame_pts_to_encoder_tb(void *logctx, AVFrame *frame,
                                             AVRational tb_dst, int64_t start_time)
{
    double float_pts = AV_NOPTS_VALUE; // this is identical to frame.pts but with higher precision

    AVRational        tb = tb_dst;
    AVRational filter_tb = frame->time_base;
    const int extra_bits = av_clip(29 - av_log2(tb.den), 0, 16);

    if (frame->pts == AV_NOPTS_VALUE)
        goto early_exit;

    tb.den <<= extra_bits;
    float_pts = av_rescale_q(frame->pts, filter_tb, tb) -
                av_rescale_q(start_time, AV_TIME_BASE_Q, tb);
    float_pts /= 1 << extra_bits;
    // when float_pts is not exactly an integer,
    // avoid exact midpoints to reduce the chance of rounding differences, this
    // can be removed in case the fps code is changed to work with integers
    if (float_pts != llrint(float_pts))
        float_pts += FFSIGN(float_pts) * 1.0 / (1<<17);

    frame->pts = av_rescale_q(frame->pts, filter_tb, tb_dst) -
                 av_rescale_q(start_time, AV_TIME_BASE_Q, tb_dst);
    frame->time_base = tb_dst;

early_exit:

    if (debug_ts) {
        av_log(logctx, AV_LOG_INFO,
               "filter -> pts:%s pts_time:%s exact:%f time_base:%d/%d\n",
               frame ? av_ts2str(frame->pts) : "NULL",
               av_ts2timestr(frame->pts, &tb_dst),
               float_pts, tb_dst.num, tb_dst.den);
    }

    return float_pts;
}

static int64_t median3(int64_t a, int64_t b, int64_t c)
{
    int64_t max2, min2, m;

    if (a >= b) {
        max2 = a;
        min2 = b;
    } else {
        max2 = b;
        min2 = a;
    }
    m = (c >= max2) ? max2 : c;

    return (m >= min2) ? m : min2;
}


/* Convert frame timestamps to the encoder timebase and decide how many times
 * should this (and possibly previous) frame be repeated in order to conform to
 * desired target framerate (if any).
 */
static void video_sync_process(OutputFilterPriv *ofp, AVFrame *frame,
                               int64_t *nb_frames, int64_t *nb_frames_prev)
{
    OutputFilter   *ofilter = &ofp->ofilter;
    FPSConvContext     *fps = &ofp->fps;
    double delta0, delta, sync_ipts, duration;

    if (!frame) {
        *nb_frames_prev = *nb_frames = median3(fps->frames_prev_hist[0],
                                               fps->frames_prev_hist[1],
                                               fps->frames_prev_hist[2]);

        if (!*nb_frames && fps->last_dropped) {
            atomic_fetch_add(&ofilter->nb_frames_drop, 1);
            fps->last_dropped++;
        }

        goto finish;
    }

    duration = frame->duration * av_q2d(frame->time_base) / av_q2d(ofp->tb_out);

    sync_ipts = adjust_frame_pts_to_encoder_tb(ofilter->graph, frame,
                                               ofp->tb_out, ofp->ts_offset);
    /* delta0 is the "drift" between the input frame and
     * where it would fall in the output. */
    delta0 = sync_ipts - ofp->next_pts;
    delta  = delta0 + duration;

    // tracks the number of times the PREVIOUS frame should be duplicated,
    // mostly for variable framerate (VFR)
    *nb_frames_prev = 0;
    /* by default, we output a single frame */
    *nb_frames = 1;

    if (delta0 < 0 &&
        delta > 0 &&
        fps->vsync_method != VSYNC_PASSTHROUGH
#if FFMPEG_OPT_VSYNC_DROP
        && fps->vsync_method != VSYNC_DROP
#endif
        ) {
        if (delta0 < -0.6) {
            av_log(ofp, AV_LOG_VERBOSE, "Past duration %f too large\n", -delta0);
        } else
            av_log(ofp, AV_LOG_DEBUG, "Clipping frame in rate conversion by %f\n", -delta0);
        sync_ipts = ofp->next_pts;
        duration += delta0;
        delta0 = 0;
    }

    switch (fps->vsync_method) {
    case VSYNC_VSCFR:
        if (fps->frame_number == 0 && delta0 >= 0.5) {
            av_log(ofp, AV_LOG_DEBUG, "Not duplicating %d initial frames\n", (int)lrintf(delta0));
            delta = duration;
            delta0 = 0;
            ofp->next_pts = llrint(sync_ipts);
        }
    case VSYNC_CFR:
        // FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
        if (frame_drop_threshold && delta < frame_drop_threshold && fps->frame_number) {
            *nb_frames = 0;
        } else if (delta < -1.1)
            *nb_frames = 0;
        else if (delta > 1.1) {
            *nb_frames = llrintf(delta);
            if (delta0 > 1.1)
                *nb_frames_prev = llrintf(delta0 - 0.6);
        }
        frame->duration = 1;
        break;
    case VSYNC_VFR:
        if (delta <= -0.6)
            *nb_frames = 0;
        else if (delta > 0.6)
            ofp->next_pts = llrint(sync_ipts);
        frame->duration = llrint(duration);
        break;
#if FFMPEG_OPT_VSYNC_DROP
    case VSYNC_DROP:
#endif
    case VSYNC_PASSTHROUGH:
        ofp->next_pts = llrint(sync_ipts);
        frame->duration = llrint(duration);
        break;
    default:
        av_assert0(0);
    }

finish:
    memmove(fps->frames_prev_hist + 1,
            fps->frames_prev_hist,
            sizeof(fps->frames_prev_hist[0]) * (FF_ARRAY_ELEMS(fps->frames_prev_hist) - 1));
    fps->frames_prev_hist[0] = *nb_frames_prev;

    if (*nb_frames_prev == 0 && fps->last_dropped) {
        atomic_fetch_add(&ofilter->nb_frames_drop, 1);
        av_log(ofp, AV_LOG_VERBOSE,
               "*** dropping frame %"PRId64" at ts %"PRId64"\n",
               fps->frame_number, fps->last_frame->pts);
    }
    if (*nb_frames > (*nb_frames_prev && fps->last_dropped) + (*nb_frames > *nb_frames_prev)) {
        uint64_t nb_frames_dup;
        if (*nb_frames > dts_error_threshold * 30) {
            av_log(ofp, AV_LOG_ERROR, "%"PRId64" frame duplication too large, skipping\n", *nb_frames - 1);
            atomic_fetch_add(&ofilter->nb_frames_drop, 1);
            *nb_frames = 0;
            return;
        }
        nb_frames_dup = atomic_fetch_add(&ofilter->nb_frames_dup,
                                         *nb_frames - (*nb_frames_prev && fps->last_dropped) - (*nb_frames > *nb_frames_prev));
        av_log(ofp, AV_LOG_VERBOSE, "*** %"PRId64" dup!\n", *nb_frames - 1);
        if (nb_frames_dup > fps->dup_warning) {
            av_log(ofp, AV_LOG_WARNING, "More than %"PRIu64" frames duplicated\n", fps->dup_warning);
            fps->dup_warning *= 10;
        }
    }

    fps->last_dropped = *nb_frames == *nb_frames_prev && frame;
    fps->dropped_keyframe |= fps->last_dropped && (frame->flags & AV_FRAME_FLAG_KEY);
}

static void close_input(InputFilterPriv *ifp)
{
    FilterGraphPriv *fgp = fgp_from_fg(ifp->ifilter.graph);

    if (!ifp->eof) {
        sch_filter_receive_finish(fgp->sch, fgp->sch_idx, ifp->ifilter.index);
        ifp->eof = 1;
    }
}

static int close_output(OutputFilterPriv *ofp, FilterGraphThread *fgt)
{
    FilterGraphPriv *fgp = fgp_from_fg(ofp->ofilter.graph);
    int ret;

    // we are finished and no frames were ever seen at this output,
    // at least initialize the encoder with a dummy frame
    if (!fgt->got_frame) {
        AVFrame *frame = fgt->frame;
        FrameData *fd;

        frame->time_base   = ofp->tb_out;
        frame->format      = ofp->format;

        frame->width               = ofp->width;
        frame->height              = ofp->height;
        frame->sample_aspect_ratio = ofp->sample_aspect_ratio;

        frame->sample_rate = ofp->sample_rate;
        if (ofp->ch_layout.nb_channels) {
            ret = av_channel_layout_copy(&frame->ch_layout, &ofp->ch_layout);
            if (ret < 0)
                return ret;
        }

        fd = frame_data(frame);
        if (!fd)
            return AVERROR(ENOMEM);

        av_frame_side_data_free(&fd->side_data, &fd->nb_side_data);
        ret = clone_side_data(&fd->side_data, &fd->nb_side_data,
                              ofp->side_data, ofp->nb_side_data, 0);
        if (ret < 0)
            return ret;

        fd->frame_rate_filter = ofp->fps.framerate;

        av_assert0(!frame->buf[0]);

        av_log(ofp, AV_LOG_WARNING,
               "No filtered frames for output stream, trying to "
               "initialize anyway.\n");

        ret = sch_filter_send(fgp->sch, fgp->sch_idx, ofp->ofilter.index, frame);
        if (ret < 0) {
            av_frame_unref(frame);
            return ret;
        }
    }

    fgt->eof_out[ofp->ofilter.index] = 1;

    ret = sch_filter_send(fgp->sch, fgp->sch_idx, ofp->ofilter.index, NULL);
    return (ret == AVERROR_EOF) ? 0 : ret;
}

static int fg_output_frame(OutputFilterPriv *ofp, FilterGraphThread *fgt,
                           AVFrame *frame)
{
    FilterGraphPriv  *fgp = fgp_from_fg(ofp->ofilter.graph);
    AVFrame   *frame_prev = ofp->fps.last_frame;
    enum AVMediaType type = ofp->ofilter.type;

    int64_t nb_frames = !!frame, nb_frames_prev = 0;

    if (type == AVMEDIA_TYPE_VIDEO && (frame || fgt->got_frame))
        video_sync_process(ofp, frame, &nb_frames, &nb_frames_prev);

    for (int64_t i = 0; i < nb_frames; i++) {
        AVFrame *frame_out;
        int ret;

        if (type == AVMEDIA_TYPE_VIDEO) {
            AVFrame *frame_in = (i < nb_frames_prev && frame_prev->buf[0]) ?
                                frame_prev : frame;
            if (!frame_in)
                break;

            frame_out = fgp->frame_enc;
            ret = av_frame_ref(frame_out, frame_in);
            if (ret < 0)
                return ret;

            frame_out->pts = ofp->next_pts;

            if (ofp->fps.dropped_keyframe) {
                frame_out->flags |= AV_FRAME_FLAG_KEY;
                ofp->fps.dropped_keyframe = 0;
            }
        } else {
            frame->pts = (frame->pts == AV_NOPTS_VALUE) ? ofp->next_pts :
                av_rescale_q(frame->pts,   frame->time_base, ofp->tb_out) -
                av_rescale_q(ofp->ts_offset, AV_TIME_BASE_Q, ofp->tb_out);

            frame->time_base = ofp->tb_out;
            frame->duration  = av_rescale_q(frame->nb_samples,
                                            (AVRational){ 1, frame->sample_rate },
                                            ofp->tb_out);

            ofp->next_pts = frame->pts + frame->duration;

            frame_out = frame;
        }

        // send the frame to consumers
        ret = sch_filter_send(fgp->sch, fgp->sch_idx, ofp->ofilter.index, frame_out);
        if (ret < 0) {
            av_frame_unref(frame_out);

            if (!fgt->eof_out[ofp->ofilter.index]) {
                fgt->eof_out[ofp->ofilter.index] = 1;
                fgp->nb_outputs_done++;
            }

            return ret == AVERROR_EOF ? 0 : ret;
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            ofp->fps.frame_number++;
            ofp->next_pts++;

            if (i == nb_frames_prev && frame)
                frame->flags &= ~AV_FRAME_FLAG_KEY;
        }

        fgt->got_frame = 1;
    }

    if (frame && frame_prev) {
        av_frame_unref(frame_prev);
        av_frame_move_ref(frame_prev, frame);
    }

    if (!frame)
        return close_output(ofp, fgt);

    return 0;
}

static int fg_output_step(OutputFilterPriv *ofp, FilterGraphThread *fgt,
                          AVFrame *frame)
{
    FilterGraphPriv    *fgp = fgp_from_fg(ofp->ofilter.graph);
    AVFilterContext *filter = ofp->ofilter.filter;
    FrameData *fd;
    int ret;

    ret = av_buffersink_get_frame_flags(filter, frame,
                                        AV_BUFFERSINK_FLAG_NO_REQUEST);
    if (ret == AVERROR_EOF && !fgt->eof_out[ofp->ofilter.index]) {
        ret = fg_output_frame(ofp, fgt, NULL);
        return (ret < 0) ? ret : 1;
    } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 1;
    } else if (ret < 0) {
        av_log(ofp, AV_LOG_WARNING,
               "Error in retrieving a frame from the filtergraph: %s\n",
               av_err2str(ret));
        return ret;
    }

    if (fgt->eof_out[ofp->ofilter.index]) {
        av_frame_unref(frame);
        return 0;
    }

    frame->time_base = av_buffersink_get_time_base(filter);

    if (debug_ts)
        av_log(ofp, AV_LOG_INFO, "filter_raw -> pts:%s pts_time:%s time_base:%d/%d\n",
               av_ts2str(frame->pts), av_ts2timestr(frame->pts, &frame->time_base),
                         frame->time_base.num, frame->time_base.den);

    // Choose the output timebase the first time we get a frame.
    if (!ofp->tb_out_locked) {
        ret = choose_out_timebase(ofp, frame);
        if (ret < 0) {
            av_log(ofp, AV_LOG_ERROR, "Could not choose an output time base\n");
            av_frame_unref(frame);
            return ret;
        }
    }

    fd = frame_data(frame);
    if (!fd) {
        av_frame_unref(frame);
        return AVERROR(ENOMEM);
    }

    av_frame_side_data_free(&fd->side_data, &fd->nb_side_data);
    if (!fgt->got_frame) {
        ret = clone_side_data(&fd->side_data, &fd->nb_side_data,
                              ofp->side_data, ofp->nb_side_data, 0);
        if (ret < 0)
            return ret;
    }

    fd->wallclock[LATENCY_PROBE_FILTER_POST] = av_gettime_relative();

    // only use bits_per_raw_sample passed through from the decoder
    // if the filtergraph did not touch the frame data
    if (!fgp->is_meta)
        fd->bits_per_raw_sample = 0;

    if (ofp->ofilter.type == AVMEDIA_TYPE_VIDEO) {
        if (!frame->duration) {
            AVRational fr = av_buffersink_get_frame_rate(filter);
            if (fr.num > 0 && fr.den > 0)
                frame->duration = av_rescale_q(1, av_inv_q(fr), frame->time_base);
        }

        fd->frame_rate_filter = ofp->fps.framerate;
    }

    ret = fg_output_frame(ofp, fgt, frame);
    av_frame_unref(frame);
    if (ret < 0)
        return ret;

    return 0;
}

/* retrieve all frames available at filtergraph outputs
 * and send them to consumers */
static int read_frames(FilterGraph *fg, FilterGraphThread *fgt,
                       AVFrame *frame)
{
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    int did_step = 0;

    // graph not configured, just select the input to request
    if (!fgt->graph) {
        for (int i = 0; i < fg->nb_inputs; i++) {
            InputFilterPriv *ifp = ifp_from_ifilter(fg->inputs[i]);
            if (ifp->format < 0 && !fgt->eof_in[i]) {
                fgt->next_in = i;
                return 0;
            }
        }

        // This state - graph is not configured, but all inputs are either
        // initialized or EOF - should be unreachable because sending EOF to a
        // filter without even a fallback format should fail
        av_assert0(0);
        return AVERROR_BUG;
    }

    while (fgp->nb_outputs_done < fg->nb_outputs) {
        int ret;

        /* Reap all buffers present in the buffer sinks */
        for (int i = 0; i < fg->nb_outputs; i++) {
            OutputFilterPriv *ofp = ofp_from_ofilter(fg->outputs[i]);

            ret = 0;
            while (!ret) {
                ret = fg_output_step(ofp, fgt, frame);
                if (ret < 0)
                    return ret;
            }
        }

        // return after one iteration, so that scheduler can rate-control us
        if (did_step && fgp->have_sources)
            return 0;

        ret = avfilter_graph_request_oldest(fgt->graph);
        if (ret == AVERROR(EAGAIN)) {
            fgt->next_in = choose_input(fg, fgt);
            return 0;
        } else if (ret < 0) {
            if (ret == AVERROR_EOF)
                av_log(fg, AV_LOG_VERBOSE, "Filtergraph returned EOF, finishing\n");
            else
                av_log(fg, AV_LOG_ERROR,
                       "Error requesting a frame from the filtergraph: %s\n",
                       av_err2str(ret));
            return ret;
        }
        fgt->next_in = fg->nb_inputs;

        did_step = 1;
    }

    return AVERROR_EOF;
}

static void sub2video_heartbeat(InputFilter *ifilter, int64_t pts, AVRational tb)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    int64_t pts2;

    /* subtitles seem to be usually muxed ahead of other streams;
       if not, subtracting a larger time here is necessary */
    pts2 = av_rescale_q(pts, tb, ifp->time_base) - 1;

    /* do not send the heartbeat frame if the subtitle is already ahead */
    if (pts2 <= ifp->sub2video.last_pts)
        return;

    if (pts2 >= ifp->sub2video.end_pts || ifp->sub2video.initialize)
        /* if we have hit the end of the current displayed subpicture,
           or if we need to initialize the system, update the
           overlaid subpicture and its start/end times */
        sub2video_update(ifp, pts2 + 1, NULL);
    else
        sub2video_push_ref(ifp, pts2);
}

static int sub2video_frame(InputFilter *ifilter, AVFrame *frame, int buffer)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    int ret;

    if (buffer) {
        AVFrame *tmp;

        if (!frame)
            return 0;

        tmp = av_frame_alloc();
        if (!tmp)
            return AVERROR(ENOMEM);

        av_frame_move_ref(tmp, frame);

        ret = av_fifo_write(ifp->frame_queue, &tmp, 1);
        if (ret < 0) {
            av_frame_free(&tmp);
            return ret;
        }

        return 0;
    }

    // heartbeat frame
    if (frame && !frame->buf[0]) {
        sub2video_heartbeat(ifilter, frame->pts, frame->time_base);
        return 0;
    }

    if (!frame) {
        if (ifp->sub2video.end_pts < INT64_MAX)
            sub2video_update(ifp, INT64_MAX, NULL);

        return av_buffersrc_add_frame(ifilter->filter, NULL);
    }

    ifp->width  = frame->width  ? frame->width  : ifp->width;
    ifp->height = frame->height ? frame->height : ifp->height;

    sub2video_update(ifp, INT64_MIN, (const AVSubtitle*)frame->buf[0]->data);

    return 0;
}

static int send_eof(FilterGraphThread *fgt, InputFilter *ifilter,
                    int64_t pts, AVRational tb)
{
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    int ret;

    if (fgt->eof_in[ifilter->index])
       return 0;

    fgt->eof_in[ifilter->index] = 1;

    if (ifilter->filter) {
        pts = av_rescale_q_rnd(pts, tb, ifp->time_base,
                               AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

        ret = av_buffersrc_close(ifilter->filter, pts, AV_BUFFERSRC_FLAG_PUSH);
        if (ret < 0)
            return ret;
    } else {
        if (ifp->format < 0) {
            // the filtergraph was never configured, use the fallback parameters
            ifp->format                 = ifp->opts.fallback->format;
            ifp->sample_rate            = ifp->opts.fallback->sample_rate;
            ifp->width                  = ifp->opts.fallback->width;
            ifp->height                 = ifp->opts.fallback->height;
            ifp->sample_aspect_ratio    = ifp->opts.fallback->sample_aspect_ratio;
            ifp->color_space            = ifp->opts.fallback->colorspace;
            ifp->color_range            = ifp->opts.fallback->color_range;
            ifp->alpha_mode             = ifp->opts.fallback->alpha_mode;
            ifp->time_base              = ifp->opts.fallback->time_base;

            ret = av_channel_layout_copy(&ifp->ch_layout,
                                         &ifp->opts.fallback->ch_layout);
            if (ret < 0)
                return ret;

            av_frame_side_data_free(&ifp->side_data, &ifp->nb_side_data);
            ret = clone_side_data(&ifp->side_data, &ifp->nb_side_data,
                                  ifp->opts.fallback->side_data,
                                  ifp->opts.fallback->nb_side_data, 0);
            if (ret < 0)
                return ret;

            if (ifilter_has_all_input_formats(ifilter->graph)) {
                ret = configure_filtergraph(ifilter->graph, fgt);
                if (ret < 0) {
                    av_log(ifilter->graph, AV_LOG_ERROR, "Error initializing filters!\n");
                    return ret;
                }
            }
        }

        if (ifp->format < 0) {
            av_log(ifilter->graph, AV_LOG_ERROR,
                   "Cannot determine format of input %s after EOF\n",
                   ifp->opts.name);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

enum ReinitReason {
    VIDEO_CHANGED   = (1 << 0),
    AUDIO_CHANGED   = (1 << 1),
    MATRIX_CHANGED  = (1 << 2),
    DOWNMIX_CHANGED = (1 << 3),
    HWACCEL_CHANGED = (1 << 4)
};

static const char *unknown_if_null(const char *str)
{
    return str ? str : "unknown";
}

static int send_frame(FilterGraph *fg, FilterGraphThread *fgt,
                      InputFilter *ifilter, AVFrame *frame)
{
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    InputFilterPriv *ifp = ifp_from_ifilter(ifilter);
    FrameData       *fd;
    AVFrameSideData *sd;
    int need_reinit = 0, ret;

    /* determine if the parameters for this input changed */
    switch (ifilter->type) {
    case AVMEDIA_TYPE_AUDIO:
        if (ifp->format      != frame->format ||
            ifp->sample_rate != frame->sample_rate ||
            av_channel_layout_compare(&ifp->ch_layout, &frame->ch_layout))
            need_reinit |= AUDIO_CHANGED;
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (ifp->format != frame->format ||
            ifp->width  != frame->width ||
            ifp->height != frame->height ||
            ifp->color_space != frame->colorspace ||
            ifp->color_range != frame->color_range ||
            ifp->alpha_mode != frame->alpha_mode)
            need_reinit |= VIDEO_CHANGED;
        break;
    }

    if (sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
        if (!ifp->displaymatrix_present ||
            memcmp(sd->data, ifp->displaymatrix, sizeof(ifp->displaymatrix)))
            need_reinit |= MATRIX_CHANGED;
    } else if (ifp->displaymatrix_present)
        need_reinit |= MATRIX_CHANGED;

    if (sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOWNMIX_INFO)) {
        if (!ifp->downmixinfo_present ||
            memcmp(sd->data, &ifp->downmixinfo, sizeof(ifp->downmixinfo)))
            need_reinit |= DOWNMIX_CHANGED;
    } else if (ifp->downmixinfo_present)
        need_reinit |= DOWNMIX_CHANGED;

    if (need_reinit && fgt->graph && (ifp->opts.flags & IFILTER_FLAG_DROPCHANGED)) {
            ifp->nb_dropped++;
            av_log_once(fg, AV_LOG_WARNING, AV_LOG_DEBUG, &ifp->drop_warned, "Avoiding reinit; dropping frame pts: %s bound for %s\n", av_ts2str(frame->pts), ifilter->name);
            av_frame_unref(frame);
            return 0;
    }

    if (!(ifp->opts.flags & IFILTER_FLAG_REINIT) && fgt->graph)
        need_reinit = 0;

    if (!!ifp->hw_frames_ctx != !!frame->hw_frames_ctx ||
        (ifp->hw_frames_ctx && ifp->hw_frames_ctx->data != frame->hw_frames_ctx->data))
        need_reinit |= HWACCEL_CHANGED;

    if (need_reinit) {
        ret = ifilter_parameters_from_frame(ifilter, frame);
        if (ret < 0)
            return ret;

        /* Inputs bound to a filtergraph output will have some fields unset.
         * Handle them here */
        if (ifp->ofilter_src) {
            ret = ifilter_parameters_from_ofilter(ifilter, ifp->ofilter_src);
            if (ret < 0)
                return ret;
        }
    }

    /* (re)init the graph if possible, otherwise buffer the frame and return */
    if (need_reinit || !fgt->graph) {
        AVFrame *tmp = av_frame_alloc();

        if (!tmp)
            return AVERROR(ENOMEM);

        if (!ifilter_has_all_input_formats(fg)) {
            av_frame_move_ref(tmp, frame);

            ret = av_fifo_write(ifp->frame_queue, &tmp, 1);
            if (ret < 0)
                av_frame_free(&tmp);

            return ret;
        }

        ret = fgt->graph ? read_frames(fg, fgt, tmp) : 0;
        av_frame_free(&tmp);
        if (ret < 0)
            return ret;

        if (fgt->graph) {
            AVBPrint reason;
            av_bprint_init(&reason, 0, AV_BPRINT_SIZE_AUTOMATIC);
            if (need_reinit & AUDIO_CHANGED) {
                const char *sample_format_name = av_get_sample_fmt_name(frame->format);
                av_bprintf(&reason, "audio parameters changed to %d Hz, ", frame->sample_rate);
                av_channel_layout_describe_bprint(&frame->ch_layout, &reason);
                av_bprintf(&reason, ", %s, ", unknown_if_null(sample_format_name));
            }
            if (need_reinit & VIDEO_CHANGED) {
                const char *pixel_format_name = av_get_pix_fmt_name(frame->format);
                const char *color_space_name = av_color_space_name(frame->colorspace);
                const char *color_range_name = av_color_range_name(frame->color_range);
                const char *alpha_mode = av_alpha_mode_name(frame->alpha_mode);
                av_bprintf(&reason, "video parameters changed to %s(%s, %s), %dx%d, %s alpha,",
                        unknown_if_null(pixel_format_name), unknown_if_null(color_range_name),
                        unknown_if_null(color_space_name), frame->width, frame->height,
                        unknown_if_null(alpha_mode));
            }
            if (need_reinit & MATRIX_CHANGED)
                av_bprintf(&reason, "display matrix changed, ");
            if (need_reinit & DOWNMIX_CHANGED)
                av_bprintf(&reason, "downmix medatata changed, ");
            if (need_reinit & HWACCEL_CHANGED)
                av_bprintf(&reason, "hwaccel changed, ");
            if (reason.len > 1)
                reason.str[reason.len - 2] = '\0'; // remove last comma
            av_log(fg, AV_LOG_INFO, "Reconfiguring filter graph%s%s\n", reason.len ? " because " : "", reason.str);
        } else {
            /* Choke all input to avoid buffering excessive frames while the
             * initial filter graph is being configured, and before we have a
             * preferred input */
            sch_filter_choke_inputs(fgp->sch, fgp->sch_idx);
        }

        ret = configure_filtergraph(fg, fgt);
        if (ret < 0) {
            av_log(fg, AV_LOG_ERROR, "Error reinitializing filters!\n");
            return ret;
        }
    }

    frame->pts       = av_rescale_q(frame->pts,      frame->time_base, ifp->time_base);
    frame->duration  = av_rescale_q(frame->duration, frame->time_base, ifp->time_base);
    frame->time_base = ifp->time_base;

    if (ifp->displaymatrix_applied)
        av_frame_remove_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);

    fd = frame_data(frame);
    if (!fd)
        return AVERROR(ENOMEM);
    fd->wallclock[LATENCY_PROBE_FILTER_PRE] = av_gettime_relative();

    ret = av_buffersrc_add_frame_flags(ifilter->filter, frame,
                                       AV_BUFFERSRC_FLAG_PUSH);
    if (ret < 0) {
        av_frame_unref(frame);
        if (ret != AVERROR_EOF)
            av_log(fg, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}

static void fg_thread_set_name(const FilterGraph *fg)
{
    char name[16];
    if (filtergraph_is_simple(fg)) {
        OutputFilterPriv *ofp = ofp_from_ofilter(fg->outputs[0]);
        snprintf(name, sizeof(name), "%cf%s",
                 av_get_media_type_string(ofp->ofilter.type)[0],
                 ofp->ofilter.output_name);
    } else {
        snprintf(name, sizeof(name), "fc%d", fg->index);
    }

    ff_thread_setname(name);
}

static void fg_thread_uninit(FilterGraphThread *fgt)
{
    if (fgt->frame_queue_out) {
        AVFrame *frame;
        while (av_fifo_read(fgt->frame_queue_out, &frame, 1) >= 0)
            av_frame_free(&frame);
        av_fifo_freep2(&fgt->frame_queue_out);
    }

    av_frame_free(&fgt->frame);
    av_freep(&fgt->eof_in);
    av_freep(&fgt->eof_out);

    avfilter_graph_free(&fgt->graph);

    memset(fgt, 0, sizeof(*fgt));
}

static int fg_thread_init(FilterGraphThread *fgt, const FilterGraph *fg)
{
    memset(fgt, 0, sizeof(*fgt));

    fgt->frame = av_frame_alloc();
    if (!fgt->frame)
        goto fail;

    fgt->eof_in = av_calloc(fg->nb_inputs, sizeof(*fgt->eof_in));
    if (!fgt->eof_in)
        goto fail;

    fgt->eof_out = av_calloc(fg->nb_outputs, sizeof(*fgt->eof_out));
    if (!fgt->eof_out)
        goto fail;

    fgt->frame_queue_out = av_fifo_alloc2(1, sizeof(AVFrame*), AV_FIFO_FLAG_AUTO_GROW);
    if (!fgt->frame_queue_out)
        goto fail;

    return 0;

fail:
    fg_thread_uninit(fgt);
    return AVERROR(ENOMEM);
}

static int filter_thread(void *arg)
{
    FilterGraphPriv *fgp = arg;
    FilterGraph      *fg = &fgp->fg;

    FilterGraphThread fgt;
    int ret = 0, input_status = 0;

    ret = fg_thread_init(&fgt, fg);
    if (ret < 0)
        goto finish;

    fg_thread_set_name(fg);

    // if we have all input parameters the graph can now be configured
    if (ifilter_has_all_input_formats(fg)) {
        ret = configure_filtergraph(fg, &fgt);
        if (ret < 0) {
            av_log(fg, AV_LOG_ERROR, "Error configuring filter graph: %s\n",
                   av_err2str(ret));
            goto finish;
        }
    }

    while (1) {
        InputFilter *ifilter;
        InputFilterPriv *ifp = NULL;
        enum FrameOpaque o;
        unsigned input_idx = fgt.next_in;

        input_status = sch_filter_receive(fgp->sch, fgp->sch_idx,
                                          &input_idx, fgt.frame);
        if (input_status == AVERROR_EOF) {
            av_log(fg, AV_LOG_VERBOSE, "Filtering thread received EOF\n");
            break;
        } else if (input_status == AVERROR(EAGAIN)) {
            // should only happen when we didn't request any input
            av_assert0(input_idx == fg->nb_inputs);
            goto read_frames;
        }
        av_assert0(input_status >= 0);

        o = (intptr_t)fgt.frame->opaque;

        o = (intptr_t)fgt.frame->opaque;

        // message on the control stream
        if (input_idx == fg->nb_inputs) {
            FilterCommand *fc;

            av_assert0(o == FRAME_OPAQUE_SEND_COMMAND && fgt.frame->buf[0]);

            fc = (FilterCommand*)fgt.frame->buf[0]->data;
            send_command(fg, fgt.graph, fc->time, fc->target, fc->command, fc->arg,
                         fc->all_filters);
            av_frame_unref(fgt.frame);
            continue;
        }

        // we received an input frame or EOF
        ifilter   = fg->inputs[input_idx];
        ifp       = ifp_from_ifilter(ifilter);

        if (ifp->type_src == AVMEDIA_TYPE_SUBTITLE) {
            int hb_frame = input_status >= 0 && o == FRAME_OPAQUE_SUB_HEARTBEAT;
            ret = sub2video_frame(ifilter, (fgt.frame->buf[0] || hb_frame) ? fgt.frame : NULL,
                                  !fgt.graph);
        } else if (fgt.frame->buf[0]) {
            ret = send_frame(fg, &fgt, ifilter, fgt.frame);
        } else {
            av_assert1(o == FRAME_OPAQUE_EOF);
            ret = send_eof(&fgt, ifilter, fgt.frame->pts, fgt.frame->time_base);
        }
        av_frame_unref(fgt.frame);
        if (ret == AVERROR_EOF) {
            av_log(fg, AV_LOG_VERBOSE, "Input %u no longer accepts new data\n",
                   input_idx);
            close_input(ifp);
            continue;
        }
        if (ret < 0)
            goto finish;

read_frames:
        // retrieve all newly available frames
        ret = read_frames(fg, &fgt, fgt.frame);
        if (ret == AVERROR_EOF) {
            av_log(fg, AV_LOG_VERBOSE, "All consumers returned EOF\n");
            if (ifp && ifp->opts.flags & IFILTER_FLAG_DROPCHANGED)
                av_log(fg, AV_LOG_INFO, "Total changed input frames dropped : %"PRId64"\n", ifp->nb_dropped);
            break;
        } else if (ret < 0) {
            av_log(fg, AV_LOG_ERROR, "Error sending frames to consumers: %s\n",
                   av_err2str(ret));
            goto finish;
        }

        // ensure all inputs no longer accepting data are closed
        for (int i = 0; fgt.graph && i < fg->nb_inputs; i++) {
            InputFilterPriv *ifp = ifp_from_ifilter(fg->inputs[i]);
            if (av_buffersrc_get_status(ifp->ifilter.filter))
                close_input(ifp);
        }
    }

    for (unsigned i = 0; i < fg->nb_outputs; i++) {
        OutputFilterPriv *ofp = ofp_from_ofilter(fg->outputs[i]);

        if (fgt.eof_out[i] || !fgt.graph)
            continue;

        ret = fg_output_frame(ofp, &fgt, NULL);
        if (ret < 0)
            goto finish;
    }

finish:

    if (print_graphs || print_graphs_file)
        print_filtergraph(fg, fgt.graph);

    // EOF is normal termination
    if (ret == AVERROR_EOF)
        ret = 0;

    fg_thread_uninit(&fgt);

    return ret;
}

void fg_send_command(FilterGraph *fg, double time, const char *target,
                     const char *command, const char *arg, int all_filters)
{
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    AVBufferRef *buf;
    FilterCommand *fc;

    fc = av_mallocz(sizeof(*fc));
    if (!fc)
        return;

    buf = av_buffer_create((uint8_t*)fc, sizeof(*fc), filter_command_free, NULL, 0);
    if (!buf) {
        av_freep(&fc);
        return;
    }

    fc->target  = av_strdup(target);
    fc->command = av_strdup(command);
    fc->arg     = av_strdup(arg);
    if (!fc->target || !fc->command || !fc->arg) {
        av_buffer_unref(&buf);
        return;
    }

    fc->time        = time;
    fc->all_filters = all_filters;

    fgp->frame->buf[0] = buf;
    fgp->frame->opaque = (void*)(intptr_t)FRAME_OPAQUE_SEND_COMMAND;

    sch_filter_command(fgp->sch, fgp->sch_idx, fgp->frame);
}


/* ========== fftools/ffmpeg_hw.c ========== */

/*
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

#include <string.h>

#include "libavutil/mem.h"

#include "ffmpeg.h"

static int nb_hw_devices;
static HWDevice **hw_devices;

HWDevice *hw_device_get_by_type(enum AVHWDeviceType type)
{
    HWDevice *found = NULL;
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        if (hw_devices[i]->type == type) {
            if (found)
                return NULL;
            found = hw_devices[i];
        }
    }
    return found;
}

HWDevice *hw_device_get_by_name(const char *name)
{
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        if (!strcmp(hw_devices[i]->name, name))
            return hw_devices[i];
    }
    return NULL;
}

static HWDevice *hw_device_add(void)
{
    int err;
    err = av_reallocp_array(&hw_devices, nb_hw_devices + 1,
                            sizeof(*hw_devices));
    if (err) {
        nb_hw_devices = 0;
        return NULL;
    }
    hw_devices[nb_hw_devices] = av_mallocz(sizeof(HWDevice));
    if (!hw_devices[nb_hw_devices])
        return NULL;
    return hw_devices[nb_hw_devices++];
}

static char *hw_device_default_name(enum AVHWDeviceType type)
{
    // Make an automatic name of the form "type%d".  We arbitrarily
    // limit at 1000 anonymous devices of the same type - there is
    // probably something else very wrong if you get to this limit.
    const char *type_name = av_hwdevice_get_type_name(type);
    char *name;
    size_t index_pos;
    int index, index_limit = 1000;
    index_pos = strlen(type_name);
    name = av_malloc(index_pos + 4);
    if (!name)
        return NULL;
    for (index = 0; index < index_limit; index++) {
        snprintf(name, index_pos + 4, "%s%d", type_name, index);
        if (!hw_device_get_by_name(name))
            break;
    }
    if (index >= index_limit) {
        av_freep(&name);
        return NULL;
    }
    return name;
}

int hw_device_init_from_string(const char *arg, HWDevice **dev_out)
{
    // "type=name"
    // "type=name,key=value,key2=value2"
    // "type=name:device,key=value,key2=value2"
    // "type:device,key=value,key2=value2"
    // -> av_hwdevice_ctx_create()
    // "type=name@name"
    // "type@name"
    // -> av_hwdevice_ctx_create_derived()

    AVDictionary *options = NULL;
    const char *type_name = NULL, *name = NULL, *device = NULL;
    enum AVHWDeviceType type;
    HWDevice *dev, *src;
    AVBufferRef *device_ref = NULL;
    int err;
    const char *errmsg, *p, *q;
    size_t k;

    k = strcspn(arg, ":=@");
    p = arg + k;

    type_name = av_strndup(arg, k);
    if (!type_name) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    type = av_hwdevice_find_type_by_name(type_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        errmsg = "unknown device type";
        goto invalid;
    }

    if (*p == '=') {
        k = strcspn(p + 1, ":@,");

        name = av_strndup(p + 1, k);
        if (!name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        if (hw_device_get_by_name(name)) {
            errmsg = "named device already exists";
            goto invalid;
        }

        p += 1 + k;
    } else {
        name = hw_device_default_name(type);
        if (!name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (!*p) {
        // New device with no parameters.
        err = av_hwdevice_ctx_create(&device_ref, type,
                                     NULL, NULL, 0);
        if (err < 0)
            goto fail;

    } else if (*p == ':') {
        // New device with some parameters.
        ++p;
        q = strchr(p, ',');
        if (q) {
            if (q - p > 0) {
                device = av_strndup(p, q - p);
                if (!device) {
                    err = AVERROR(ENOMEM);
                    goto fail;
                }
            }
            err = av_dict_parse_string(&options, q + 1, "=", ",", 0);
            if (err < 0) {
                errmsg = "failed to parse options";
                goto invalid;
            }
        }

        err = av_hwdevice_ctx_create(&device_ref, type,
                                     q ? device : p[0] ? p : NULL,
                                     options, 0);
        if (err < 0)
            goto fail;

    } else if (*p == '@') {
        // Derive from existing device.

        src = hw_device_get_by_name(p + 1);
        if (!src) {
            errmsg = "invalid source device name";
            goto invalid;
        }

        err = av_hwdevice_ctx_create_derived(&device_ref, type,
                                             src->device_ref, 0);
        if (err < 0)
            goto fail;
    } else if (*p == ',') {
        err = av_dict_parse_string(&options, p + 1, "=", ",", 0);

        if (err < 0) {
            errmsg = "failed to parse options";
            goto invalid;
        }

        err = av_hwdevice_ctx_create(&device_ref, type,
                                     NULL, options, 0);
        if (err < 0)
            goto fail;
    } else {
        errmsg = "parse error";
        goto invalid;
    }

    dev = hw_device_add();
    if (!dev) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    dev->name = name;
    dev->type = type;
    dev->device_ref = device_ref;

    if (dev_out)
        *dev_out = dev;

    name = NULL;
    err = 0;
done:
    av_freep(&type_name);
    av_freep(&name);
    av_freep(&device);
    av_dict_free(&options);
    return err;
invalid:
    av_log(NULL, AV_LOG_ERROR,
           "Invalid device specification \"%s\": %s\n", arg, errmsg);
    err = AVERROR(EINVAL);
    goto done;
fail:
    av_log(NULL, AV_LOG_ERROR,
           "Device creation failed: %d.\n", err);
    av_buffer_unref(&device_ref);
    goto done;
}

int hw_device_init_from_type(enum AVHWDeviceType type,
                             const char *device,
                             HWDevice **dev_out)
{
    AVBufferRef *device_ref = NULL;
    HWDevice *dev;
    char *name;
    int err;

    name = hw_device_default_name(type);
    if (!name) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_hwdevice_ctx_create(&device_ref, type, device, NULL, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Device creation failed: %d.\n", err);
        goto fail;
    }

    dev = hw_device_add();
    if (!dev) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    dev->name = name;
    dev->type = type;
    dev->device_ref = device_ref;

    if (dev_out)
        *dev_out = dev;

    return 0;

fail:
    av_freep(&name);
    av_buffer_unref(&device_ref);
    return err;
}

void hw_device_free_all(void)
{
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        av_freep(&hw_devices[i]->name);
        av_buffer_unref(&hw_devices[i]->device_ref);
        av_freep(&hw_devices[i]);
    }
    av_freep(&hw_devices);
    nb_hw_devices = 0;
}

AVBufferRef *hw_device_for_filter(void)
{
    // Pick the last hardware device if the user doesn't pick the device for
    // filters explicitly with the filter_hw_device option.
    if (filter_hw_device)
        return filter_hw_device->device_ref;
    else if (nb_hw_devices > 0) {
        HWDevice *dev = hw_devices[nb_hw_devices - 1];

        if (nb_hw_devices > 1)
            av_log(NULL, AV_LOG_WARNING, "There are %d hardware devices. device "
                   "%s of type %s is picked for filters by default. Set hardware "
                   "device explicitly with the filter_hw_device option if device "
                   "%s is not usable for filters.\n",
                   nb_hw_devices, dev->name,
                   av_hwdevice_get_type_name(dev->type), dev->name);

        return dev->device_ref;
    }

    return NULL;
}


/* ========== fftools/ffmpeg_mux.c ========== */

/*
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

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ffmpeg.h"
#include "ffmpeg_mux.h"
#include "ffmpeg_utils.h"
#include "sync_queue.h"

#include "libavutil/avstring.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/packet.h"

#include "libavformat/avformat.h"
#include "libavformat/avio.h"

typedef struct MuxThreadContext {
    AVPacket *pkt;
    AVPacket *fix_sub_duration_pkt;
} MuxThreadContext;

static Muxer *mux_from_of(OutputFile *of)
{
    return (Muxer*)of;
}

static int64_t filesize(AVIOContext *pb)
{
    int64_t ret = -1;

    if (pb) {
        ret = avio_size(pb);
        if (ret <= 0) // FIXME improve avio_size() so it works with non seekable output too
            ret = avio_tell(pb);
    }

    return ret;
}

static void mux_log_debug_ts(OutputStream *ost, const AVPacket *pkt)
{
    static const char *desc[] = {
        [LATENCY_PROBE_DEMUX]       = "demux",
        [LATENCY_PROBE_DEC_PRE]     = "decode",
        [LATENCY_PROBE_DEC_POST]    = "decode",
        [LATENCY_PROBE_FILTER_PRE]  = "filter",
        [LATENCY_PROBE_FILTER_POST] = "filter",
        [LATENCY_PROBE_ENC_PRE]     = "encode",
        [LATENCY_PROBE_ENC_POST]    = "encode",
        [LATENCY_PROBE_NB]          = "mux",
    };

    char latency[512];

    *latency = 0;
    if (pkt->opaque_ref) {
        const FrameData *fd = (FrameData*)pkt->opaque_ref->data;
        int64_t         now = av_gettime_relative();
        int64_t       total = INT64_MIN;

        int next;

        for (unsigned i = 0; i < FF_ARRAY_ELEMS(fd->wallclock); i = next) {
            int64_t val = fd->wallclock[i];

            next = i + 1;

            if (val == INT64_MIN)
                continue;

            if (total == INT64_MIN) {
                total = now - val;
                snprintf(latency, sizeof(latency), "total:%gms", total / 1e3);
            }

            // find the next valid entry
            for (; next <= FF_ARRAY_ELEMS(fd->wallclock); next++) {
                int64_t val_next = (next == FF_ARRAY_ELEMS(fd->wallclock)) ?
                                   now : fd->wallclock[next];
                int64_t diff;

                if (val_next == INT64_MIN)
                    continue;
                diff = val_next - val;

                // print those stages that take at least 5% of total
                if (100. * diff > 5. * total) {
                    av_strlcat(latency, ", ", sizeof(latency));

                    if (!strcmp(desc[i], desc[next]))
                        av_strlcat(latency, desc[i], sizeof(latency));
                    else
                        av_strlcatf(latency, sizeof(latency), "%s-%s:",
                                    desc[i], desc[next]);

                    av_strlcatf(latency, sizeof(latency), " %gms/%d%%",
                                diff / 1e3, (int)(100. * diff / total));
                }

                break;
            }

        }
    }

    av_log(ost, AV_LOG_INFO, "muxer <- pts:%s pts_time:%s dts:%s dts_time:%s "
           "duration:%s duration_time:%s size:%d latency(%s)\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &ost->st->time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &ost->st->time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &ost->st->time_base),
           pkt->size, *latency ? latency : "N/A");
}

static int mux_fixup_ts(Muxer *mux, MuxStream *ms, AVPacket *pkt)
{
    OutputStream *ost = &ms->ost;

#if FFMPEG_OPT_VSYNC_DROP
    if (ost->type == AVMEDIA_TYPE_VIDEO && ms->ts_drop)
        pkt->pts = pkt->dts = AV_NOPTS_VALUE;
#endif

    // rescale timestamps to the stream timebase
    if (ost->type == AVMEDIA_TYPE_AUDIO && !ost->enc) {
        // use av_rescale_delta() for streamcopying audio, to preserve
        // accuracy with coarse input timebases
        int duration = av_get_audio_frame_duration2(ost->st->codecpar, pkt->size);

        if (!duration)
            duration = ost->st->codecpar->frame_size;

        pkt->dts = av_rescale_delta(pkt->time_base, pkt->dts,
                                    (AVRational){1, ost->st->codecpar->sample_rate}, duration,
                                    &ms->ts_rescale_delta_last, ost->st->time_base);
        pkt->pts = pkt->dts;

        pkt->duration = av_rescale_q(pkt->duration, pkt->time_base, ost->st->time_base);
    } else
        av_packet_rescale_ts(pkt, pkt->time_base, ost->st->time_base);
    pkt->time_base = ost->st->time_base;

    if (!(mux->fc->oformat->flags & AVFMT_NOTIMESTAMPS)) {
        if (pkt->dts != AV_NOPTS_VALUE &&
            pkt->pts != AV_NOPTS_VALUE &&
            pkt->dts > pkt->pts) {
            av_log(ost, AV_LOG_WARNING, "Invalid DTS: %"PRId64" PTS: %"PRId64", replacing by guess\n",
                   pkt->dts, pkt->pts);
            pkt->pts =
            pkt->dts = pkt->pts + pkt->dts + ms->last_mux_dts + 1
                     - FFMIN3(pkt->pts, pkt->dts, ms->last_mux_dts + 1)
                     - FFMAX3(pkt->pts, pkt->dts, ms->last_mux_dts + 1);
        }
        if ((ost->type == AVMEDIA_TYPE_AUDIO || ost->type == AVMEDIA_TYPE_VIDEO || ost->type == AVMEDIA_TYPE_SUBTITLE) &&
            pkt->dts != AV_NOPTS_VALUE &&
            ms->last_mux_dts != AV_NOPTS_VALUE) {
            int64_t max = ms->last_mux_dts + !(mux->fc->oformat->flags & AVFMT_TS_NONSTRICT);
            if (pkt->dts < max) {
                int loglevel = max - pkt->dts > 2 || ost->type == AVMEDIA_TYPE_VIDEO ? AV_LOG_WARNING : AV_LOG_DEBUG;
                if (exit_on_error)
                    loglevel = AV_LOG_ERROR;
                av_log(ost, loglevel, "Non-monotonic DTS; "
                       "previous: %"PRId64", current: %"PRId64"; ",
                       ms->last_mux_dts, pkt->dts);
                if (exit_on_error) {
                    return AVERROR(EINVAL);
                }

                av_log(ost, loglevel, "changing to %"PRId64". This may result "
                       "in incorrect timestamps in the output file.\n",
                       max);
                if (pkt->pts >= pkt->dts)
                    pkt->pts = FFMAX(pkt->pts, max);
                pkt->dts = max;
            }
        }
    }
    ms->last_mux_dts = pkt->dts;

    if (debug_ts)
        mux_log_debug_ts(ost, pkt);

    return 0;
}

static int write_packet(Muxer *mux, OutputStream *ost, AVPacket *pkt)
{
    MuxStream *ms = ms_from_ost(ost);
    AVFormatContext *s = mux->fc;
    int64_t fs;
    uint64_t frame_num;
    int ret;

    fs = filesize(s->pb);
    atomic_store(&mux->last_filesize, fs);
    if (fs >= mux->limit_filesize) {
        ret = AVERROR_EOF;
        goto fail;
    }

    ret = mux_fixup_ts(mux, ms, pkt);
    if (ret < 0)
        goto fail;

    ms->data_size_mux += pkt->size;
    frame_num = atomic_fetch_add(&ost->packets_written, 1);

    pkt->stream_index = ost->index;

    if (ms->stats.io)
        enc_stats_write(ost, &ms->stats, NULL, pkt, frame_num);

    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        av_log(ost, AV_LOG_ERROR,
               "Error submitting a packet to the muxer: %s\n",
               av_err2str(ret));
        goto fail;
    }

    return 0;
fail:
    av_packet_unref(pkt);
    return ret;
}

static int sync_queue_process(Muxer *mux, MuxStream *ms, AVPacket *pkt, int *stream_eof)
{
    OutputFile *of = &mux->of;

    if (ms->sq_idx_mux >= 0) {
        int ret = sq_send(mux->sq_mux, ms->sq_idx_mux, SQPKT(pkt));
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                *stream_eof = 1;

            return ret;
        }

        while (1) {
            ret = sq_receive(mux->sq_mux, -1, SQPKT(mux->sq_pkt));
            if (ret < 0) {
                /* n.b.: We forward EOF from the sync queue, terminating muxing.
                 * This assumes that if a muxing sync queue is present, then all
                 * the streams use it. That is true currently, but may change in
                 * the future, then this code needs to be revisited.
                 */
                return ret == AVERROR(EAGAIN) ? 0 : ret;
            }

            ret = write_packet(mux, of->streams[ret],
                               mux->sq_pkt);
            if (ret < 0)
                return ret;
        }
    } else if (pkt)
        return write_packet(mux, &ms->ost, pkt);

    return 0;
}

static int of_streamcopy(OutputFile *of, OutputStream *ost, AVPacket *pkt);

/* apply the output bitstream filters */
static int mux_packet_filter(Muxer *mux, MuxThreadContext *mt,
                             OutputStream *ost, AVPacket *pkt, int *stream_eof)
{
    MuxStream *ms = ms_from_ost(ost);
    const char *err_msg;
    int ret;

    if (pkt && !ost->enc) {
        ret = of_streamcopy(&mux->of, ost, pkt);
        if (ret == AVERROR(EAGAIN))
            return 0;
        else if (ret == AVERROR_EOF) {
            av_packet_unref(pkt);
            pkt = NULL;
            *stream_eof = 1;
        } else if (ret < 0)
            goto fail;
    }

    // emit heartbeat for -fix_sub_duration;
    // we are only interested in heartbeats on on random access points.
    if (pkt && (pkt->flags & AV_PKT_FLAG_KEY)) {
        mt->fix_sub_duration_pkt->opaque    = (void*)(intptr_t)PKT_OPAQUE_FIX_SUB_DURATION;
        mt->fix_sub_duration_pkt->pts       = pkt->pts;
        mt->fix_sub_duration_pkt->time_base = pkt->time_base;

        ret = sch_mux_sub_heartbeat(mux->sch, mux->sch_idx, ms->sch_idx,
                                    mt->fix_sub_duration_pkt);
        if (ret < 0)
            goto fail;
    }

    if (ms->bsf_ctx) {
        int bsf_eof = 0;

        if (pkt)
            av_packet_rescale_ts(pkt, pkt->time_base, ms->bsf_ctx->time_base_in);

        ret = av_bsf_send_packet(ms->bsf_ctx, pkt);
        if (ret < 0) {
            err_msg = "submitting a packet for bitstream filtering";
            goto fail;
        }

        while (!bsf_eof) {
            ret = av_bsf_receive_packet(ms->bsf_ctx, ms->bsf_pkt);
            if (ret == AVERROR(EAGAIN))
                return 0;
            else if (ret == AVERROR_EOF)
                bsf_eof = 1;
            else if (ret < 0) {
                av_log(ost, AV_LOG_ERROR,
                       "Error applying bitstream filters to a packet: %s",
                       av_err2str(ret));
                if (exit_on_error)
                    return ret;
                continue;
            }

            if (!bsf_eof)
                ms->bsf_pkt->time_base = ms->bsf_ctx->time_base_out;

            ret = sync_queue_process(mux, ms, bsf_eof ? NULL : ms->bsf_pkt, stream_eof);
            if (ret < 0)
                goto mux_fail;
        }
        *stream_eof = 1;
    } else {
        ret = sync_queue_process(mux, ms, pkt, stream_eof);
        if (ret < 0)
            goto mux_fail;
    }

    return *stream_eof ? AVERROR_EOF : 0;

mux_fail:
    err_msg = "submitting a packet to the muxer";

fail:
    if (ret != AVERROR_EOF)
        av_log(ost, AV_LOG_ERROR, "Error %s: %s\n", err_msg, av_err2str(ret));
    return ret;
}

static void thread_set_name(Muxer *mux)
{
    char name[16];
    snprintf(name, sizeof(name), "mux%d:%s",
             mux->of.index, mux->fc->oformat->name);
    ff_thread_setname(name);
}

static void mux_thread_uninit(MuxThreadContext *mt)
{
    av_packet_free(&mt->pkt);
    av_packet_free(&mt->fix_sub_duration_pkt);

    memset(mt, 0, sizeof(*mt));
}

static int mux_thread_init(MuxThreadContext *mt)
{
    memset(mt, 0, sizeof(*mt));

    mt->pkt = av_packet_alloc();
    if (!mt->pkt)
        goto fail;

    mt->fix_sub_duration_pkt = av_packet_alloc();
    if (!mt->fix_sub_duration_pkt)
        goto fail;

    return 0;

fail:
    mux_thread_uninit(mt);
    return AVERROR(ENOMEM);
}

int muxer_thread(void *arg)
{
    Muxer     *mux = arg;
    OutputFile *of = &mux->of;

    MuxThreadContext mt;

    int        ret = 0;

    ret = mux_thread_init(&mt);
    if (ret < 0)
        goto finish;

    thread_set_name(mux);

    while (1) {
        OutputStream *ost;
        int stream_idx, stream_eof = 0;

        ret = sch_mux_receive(mux->sch, of->index, mt.pkt);
        stream_idx = mt.pkt->stream_index;
        if (stream_idx < 0) {
            av_log(mux, AV_LOG_VERBOSE, "All streams finished\n");
            ret = 0;
            break;
        }

        ost = of->streams[mux->sch_stream_idx[stream_idx]];
        mt.pkt->stream_index = ost->index;
        mt.pkt->flags       &= ~AV_PKT_FLAG_TRUSTED;

        ret = mux_packet_filter(mux, &mt, ost, ret < 0 ? NULL : mt.pkt, &stream_eof);
        av_packet_unref(mt.pkt);
        if (ret == AVERROR_EOF) {
            if (stream_eof) {
                sch_mux_receive_finish(mux->sch, of->index, stream_idx);
            } else {
                av_log(mux, AV_LOG_VERBOSE, "Muxer returned EOF\n");
                ret = 0;
                break;
            }
        } else if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error muxing a packet\n");
            break;
        }
    }

finish:
    mux_thread_uninit(&mt);

    return ret;
}

static int of_streamcopy(OutputFile *of, OutputStream *ost, AVPacket *pkt)
{
    MuxStream  *ms = ms_from_ost(ost);
    FrameData  *fd = pkt->opaque_ref ? (FrameData*)pkt->opaque_ref->data : NULL;
    int64_t      dts = fd ? fd->dts_est : AV_NOPTS_VALUE;
    int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
    int64_t ts_offset;

    if (of->recording_time != INT64_MAX &&
        dts >= of->recording_time + start_time)
        return AVERROR_EOF;

    if (!ms->streamcopy_started && !(pkt->flags & AV_PKT_FLAG_KEY) &&
        !ms->copy_initial_nonkeyframes)
        return AVERROR(EAGAIN);

    if (!ms->streamcopy_started) {
        if (!ms->copy_prior_start &&
            (pkt->pts == AV_NOPTS_VALUE ?
             dts < ms->ts_copy_start :
             pkt->pts < av_rescale_q(ms->ts_copy_start, AV_TIME_BASE_Q, pkt->time_base)))
            return AVERROR(EAGAIN);

        if (of->start_time != AV_NOPTS_VALUE && dts < of->start_time)
            return AVERROR(EAGAIN);
    }

    ts_offset = av_rescale_q(start_time, AV_TIME_BASE_Q, pkt->time_base);

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= ts_offset;

    if (pkt->dts == AV_NOPTS_VALUE) {
        pkt->dts = av_rescale_q(dts, AV_TIME_BASE_Q, pkt->time_base);
    } else if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        pkt->pts = pkt->dts - ts_offset;
    }

    pkt->dts -= ts_offset;

    ms->streamcopy_started = 1;

    return 0;
}

int print_sdp(const char *filename);

int print_sdp(const char *filename)
{
    char sdp[16384];
    int j = 0, ret;
    AVIOContext *sdp_pb;
    AVFormatContext **avc;

    avc = av_malloc_array(nb_output_files, sizeof(*avc));
    if (!avc)
        return AVERROR(ENOMEM);
    for (int i = 0; i < nb_output_files; i++) {
        Muxer *mux = mux_from_of(output_files[i]);

        if (!strcmp(mux->fc->oformat->name, "rtp")) {
            avc[j] = mux->fc;
            j++;
        }
    }

    if (!j) {
        av_log(NULL, AV_LOG_ERROR, "No output streams in the SDP.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = av_sdp_create(avc, j, sdp, sizeof(sdp));
    if (ret < 0)
        goto fail;

    if (!filename) {
        printf("SDP:\n%s\n", sdp);
        fflush(stdout);
    } else {
        ret = avio_open2(&sdp_pb, filename, AVIO_FLAG_WRITE, &int_cb, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open sdp file '%s'\n", filename);
            goto fail;
        }

        avio_print(sdp_pb, sdp);
        avio_closep(&sdp_pb);
    }

fail:
    av_freep(&avc);
    return ret;
}

int mux_check_init(void *arg)
{
    Muxer     *mux = arg;
    OutputFile *of = &mux->of;
    AVFormatContext *fc = mux->fc;
    int ret;

    ret = avformat_write_header(fc, &mux->opts);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Could not write header (incorrect codec "
               "parameters ?): %s\n", av_err2str(ret));
        return ret;
    }
    //assert_avoptions(of->opts);
    mux->header_written = 1;

    av_dump_format(fc, of->index, fc->url, 1);
    atomic_fetch_add(&nb_output_dumped, 1);

    return 0;
}

static int bsf_init(MuxStream *ms)
{
    OutputStream *ost = &ms->ost;
    AVBSFContext *ctx = ms->bsf_ctx;
    int ret;

    if (!ctx)
        return avcodec_parameters_copy(ost->st->codecpar, ms->par_in);

    ret = avcodec_parameters_copy(ctx->par_in, ms->par_in);
    if (ret < 0)
        return ret;

    ctx->time_base_in = ost->st->time_base;

    ret = av_bsf_init(ctx);
    if (ret < 0) {
        av_log(ms, AV_LOG_ERROR, "Error initializing bitstream filter: %s\n",
               ctx->filter->name);
        return ret;
    }

    ret = avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
    if (ret < 0)
        return ret;
    ost->st->time_base = ctx->time_base_out;

    ms->bsf_pkt = av_packet_alloc();
    if (!ms->bsf_pkt)
        return AVERROR(ENOMEM);

    return 0;
}

int of_stream_init(OutputFile *of, OutputStream *ost,
                   const AVCodecContext *enc_ctx)
{
    Muxer *mux = mux_from_of(of);
    MuxStream *ms = ms_from_ost(ost);
    int ret;

    if (enc_ctx) {
        // use upstream time base unless it has been overridden previously
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(enc_ctx->time_base, (AVRational){0, 1});

        ost->st->avg_frame_rate = enc_ctx->framerate;
        ost->st->sample_aspect_ratio = enc_ctx->sample_aspect_ratio;

        ret = avcodec_parameters_from_context(ms->par_in, enc_ctx);
        if (ret < 0) {
            av_log(ost, AV_LOG_FATAL,
                   "Error initializing the output stream codec parameters.\n");
            return ret;
        }
    }

    /* initialize bitstream filters for the output stream
     * needs to be done here, because the codec id for streamcopy is not
     * known until now */
    ret = bsf_init(ms);
    if (ret < 0)
        return ret;

    if (ms->stream_duration) {
        ost->st->duration = av_rescale_q(ms->stream_duration, ms->stream_duration_tb,
                                         ost->st->time_base);
    }

    if (ms->sch_idx >= 0)
        return sch_mux_stream_ready(mux->sch, of->index, ms->sch_idx);

    return 0;
}

static int check_written(OutputFile *of)
{
    int64_t total_packets_written = 0;
    int pass1_used = 1;
    int ret = 0;

    for (int i = 0; i < of->nb_streams; i++) {
        OutputStream *ost = of->streams[i];
        uint64_t packets_written = atomic_load(&ost->packets_written);

        total_packets_written += packets_written;

        if (ost->enc &&
            (ost->enc->enc_ctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2))
             != AV_CODEC_FLAG_PASS1)
            pass1_used = 0;

        if (!packets_written &&
            (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT_STREAM)) {
            av_log(ost, AV_LOG_FATAL, "Empty output stream\n");
            ret = err_merge(ret, AVERROR(EINVAL));
        }
    }

    if (!total_packets_written) {
        int level = AV_LOG_WARNING;

        if (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT) {
            ret = err_merge(ret, AVERROR(EINVAL));
            level = AV_LOG_FATAL;
        }

        av_log(of, level, "Output file is empty, nothing was encoded%s\n",
               pass1_used ? "" : "(check -ss / -t / -frames parameters if used)");
    }

    return ret;
}

static void mux_final_stats(Muxer *mux)
{
    OutputFile *of = &mux->of;
    uint64_t total_packets = 0, total_size = 0;
    uint64_t video_size = 0, audio_size = 0, subtitle_size = 0,
             extra_size = 0, other_size = 0;

    uint8_t overhead[16] = "unknown";
    int64_t file_size = of_filesize(of);

    av_log(of, AV_LOG_VERBOSE, "Output file #%d (%s):\n",
           of->index, of->url);

    for (int j = 0; j < of->nb_streams; j++) {
        OutputStream *ost = of->streams[j];
        MuxStream     *ms = ms_from_ost(ost);
        const AVCodecParameters *par = ost->st->codecpar;
        const  enum AVMediaType type = par->codec_type;
        const uint64_t s = ms->data_size_mux;

        switch (type) {
        case AVMEDIA_TYPE_VIDEO:    video_size    += s; break;
        case AVMEDIA_TYPE_AUDIO:    audio_size    += s; break;
        case AVMEDIA_TYPE_SUBTITLE: subtitle_size += s; break;
        default:                    other_size    += s; break;
        }

        extra_size    += par->extradata_size;
        total_size    += s;
        total_packets += atomic_load(&ost->packets_written);

        av_log(of, AV_LOG_VERBOSE, "  Output stream #%d:%d (%s): ",
               of->index, j, av_get_media_type_string(type));
        if (ost->enc) {
            av_log(of, AV_LOG_VERBOSE, "%"PRIu64" frames encoded",
                   ost->enc->frames_encoded);
            if (type == AVMEDIA_TYPE_AUDIO)
                av_log(of, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ost->enc->samples_encoded);
            av_log(of, AV_LOG_VERBOSE, "; ");
        }

        av_log(of, AV_LOG_VERBOSE, "%"PRIu64" packets muxed (%"PRIu64" bytes); ",
               atomic_load(&ost->packets_written), s);

        av_log(of, AV_LOG_VERBOSE, "\n");
    }

    av_log(of, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) muxed\n",
           total_packets, total_size);

    if (total_size && file_size > 0 && file_size >= total_size) {
        snprintf(overhead, sizeof(overhead), "%f%%",
                 100.0 * (file_size - total_size) / total_size);
    }

    av_log(of, AV_LOG_INFO,
           "video:%1.0fKiB audio:%1.0fKiB subtitle:%1.0fKiB other streams:%1.0fKiB "
           "global headers:%1.0fKiB muxing overhead: %s\n",
           video_size    / 1024.0,
           audio_size    / 1024.0,
           subtitle_size / 1024.0,
           other_size    / 1024.0,
           extra_size    / 1024.0,
           overhead);
}

int of_write_trailer(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux->fc;
    int ret, mux_result = 0;

    if (!mux->header_written) {
        av_log(mux, AV_LOG_ERROR,
               "Nothing was written into output file, because "
               "at least one of its streams received no packets.\n");
        return AVERROR(EINVAL);
    }

    ret = av_write_trailer(fc);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Error writing trailer: %s\n", av_err2str(ret));
        mux_result = err_merge(mux_result, ret);
    }

    mux->last_filesize = filesize(fc->pb);

    if (!(fc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_closep(&fc->pb);
        if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error closing file: %s\n", av_err2str(ret));
            mux_result = err_merge(mux_result, ret);
        }
    }

    mux_final_stats(mux);

    // check whether anything was actually written
    ret = check_written(of);
    mux_result = err_merge(mux_result, ret);

    return mux_result;
}

static void enc_stats_uninit(EncStats *es)
{
    for (int i = 0; i < es->nb_components; i++)
        av_freep(&es->components[i].str);
    av_freep(&es->components);

    if (es->lock_initialized)
        pthread_mutex_destroy(&es->lock);
    es->lock_initialized = 0;
}

static void ost_free(OutputStream **post)
{
    OutputStream *ost = *post;
    MuxStream *ms;

    if (!ost)
        return;
    ms = ms_from_ost(ost);

    enc_free(&ost->enc);
    fg_free(&ost->fg_simple);

    if (ost->logfile) {
        if (fclose(ost->logfile))
            av_log(ms, AV_LOG_ERROR,
                   "Error closing logfile, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
        ost->logfile = NULL;
    }

    avcodec_parameters_free(&ms->par_in);

    av_bsf_free(&ms->bsf_ctx);
    av_packet_free(&ms->bsf_pkt);

    av_packet_free(&ms->pkt);

    av_freep(&ost->kf.pts);
    av_expr_free(ost->kf.pexpr);

    av_freep(&ost->logfile_prefix);

    av_freep(&ost->attachment_filename);

    enc_stats_uninit(&ost->enc_stats_pre);
    enc_stats_uninit(&ost->enc_stats_post);
    enc_stats_uninit(&ms->stats);

    av_freep(post);
}

static void fc_close(AVFormatContext **pfc)
{
    AVFormatContext *fc = *pfc;

    if (!fc)
        return;

    if (!(fc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fc->pb);
    avformat_free_context(fc);

    *pfc = NULL;
}

void of_free(OutputFile **pof)
{
    OutputFile *of = *pof;
    Muxer *mux;

    if (!of)
        return;
    mux = mux_from_of(of);

    sq_free(&mux->sq_mux);

    for (int i = 0; i < of->nb_streams; i++)
        ost_free(&of->streams[i]);
    av_freep(&of->streams);

    av_freep(&mux->sch_stream_idx);

    av_dict_free(&mux->opts);
    av_dict_free(&mux->enc_opts_used);

    av_packet_free(&mux->sq_pkt);

    fc_close(&mux->fc);

    av_freep(pof);
}

int64_t of_filesize(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    return atomic_load(&mux->last_filesize);
}


/* ========== fftools/ffmpeg_mux_init.c ========== */

/*
 * Muxer/output file setup.
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

#include <string.h>

#include "cmdutils.h"
#include "ffmpeg.h"
#include "ffmpeg_mux.h"
#include "ffmpeg_sched.h"
#include "fopen_utf8.h"

#include "libavformat/avformat.h"
#include "libavformat/avio.h"

#include "libavcodec/avcodec.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/display.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/iamf.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"

#define DEFAULT_PASS_LOGFILENAME_PREFIX "ffmpeg2pass"

static int check_opt_bitexact(void *ctx, const AVDictionary *opts,
                              const char *opt_name, int flag)
{
    const AVDictionaryEntry *e = av_dict_get(opts, opt_name, NULL, 0);

    if (e) {
        const AVOption *o = av_opt_find(ctx, opt_name, NULL, 0, 0);
        int val = 0;
        if (!o)
            return 0;
        av_opt_eval_flags(ctx, o, e->value, &val);
        return !!(val & flag);
    }
    return 0;
}

static int choose_encoder(const OptionsContext *o, AVFormatContext *s,
                          MuxStream *ms, const AVCodec **enc)
{
    OutputStream     *ost = &ms->ost;
    enum AVMediaType type = ost->type;
    const char *codec_name = NULL;

    *enc = NULL;

    opt_match_per_stream_str(ost, &o->codec_names, s, ost->st, &codec_name);

    if (type != AVMEDIA_TYPE_VIDEO      &&
        type != AVMEDIA_TYPE_AUDIO      &&
        type != AVMEDIA_TYPE_SUBTITLE) {
        if (codec_name && strcmp(codec_name, "copy")) {
            const char *type_str = av_get_media_type_string(type);
            av_log(ost, AV_LOG_FATAL,
                   "Encoder '%s' specified, but only '-codec copy' supported "
                   "for %s streams\n", codec_name, type_str);
            return AVERROR(ENOSYS);
        }
        return 0;
    }

    if (!codec_name) {
        ms->par_in->codec_id = av_guess_codec(s->oformat, NULL, s->url, NULL, ost->type);
        *enc = avcodec_find_encoder(ms->par_in->codec_id);
        if (!*enc) {
            av_log(ost, AV_LOG_FATAL, "Automatic encoder selection failed "
                   "Default encoder for format %s (codec %s) is "
                   "probably disabled. Please choose an encoder manually.\n",
                    s->oformat->name, avcodec_get_name(ms->par_in->codec_id));
            return AVERROR_ENCODER_NOT_FOUND;
        }
    } else if (strcmp(codec_name, "copy")) {
        int ret = find_codec(ost, codec_name, ost->type, 1, enc);
        if (ret < 0)
            return ret;
        ms->par_in->codec_id = (*enc)->id;
    }

    return 0;
}

static char *get_line(AVIOContext *s, AVBPrint *bprint)
{
    char c;

    while ((c = avio_r8(s)) && c != '\n')
        av_bprint_chars(bprint, c, 1);

    if (!av_bprint_is_complete(bprint))
        return NULL;

    return bprint->str;
}

static int get_preset_file_2(const char *preset_name, const char *codec_name, AVIOContext **s)
{
    int i, ret = -1;
    char filename[1000];
    char *env_avconv_datadir = getenv_utf8("AVCONV_DATADIR");
    char *env_home = getenv_utf8("HOME");
    const char *base[3] = { env_avconv_datadir,
                            env_home,
                            AVCONV_DATADIR,
                            };

    for (i = 0; i < FF_ARRAY_ELEMS(base) && ret < 0; i++) {
        if (!base[i])
            continue;
        if (codec_name) {
            snprintf(filename, sizeof(filename), "%s%s/%s-%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", codec_name, preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
        if (ret < 0) {
            snprintf(filename, sizeof(filename), "%s%s/%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
    }
    freeenv_utf8(env_home);
    freeenv_utf8(env_avconv_datadir);
    return ret;
}

typedef struct EncStatsFile {
    char        *path;
    AVIOContext *io;
} EncStatsFile;

static EncStatsFile   *enc_stats_files;
static          int nb_enc_stats_files;

static int enc_stats_get_file(AVIOContext **io, const char *path)
{
    EncStatsFile *esf;
    int ret;

    for (int i = 0; i < nb_enc_stats_files; i++)
        if (!strcmp(path, enc_stats_files[i].path)) {
            *io = enc_stats_files[i].io;
            return 0;
        }

    ret = GROW_ARRAY(enc_stats_files, nb_enc_stats_files);
    if (ret < 0)
        return ret;

    esf = &enc_stats_files[nb_enc_stats_files - 1];

    ret = avio_open2(&esf->io, path, AVIO_FLAG_WRITE, &int_cb, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error opening stats file '%s': %s\n",
               path, av_err2str(ret));
        return ret;
    }

    esf->path = av_strdup(path);
    if (!esf->path)
        return AVERROR(ENOMEM);

    *io = esf->io;

    return 0;
}

void of_enc_stats_close(void)
{
    for (int i = 0; i < nb_enc_stats_files; i++) {
        av_freep(&enc_stats_files[i].path);
        avio_closep(&enc_stats_files[i].io);
    }
    av_freep(&enc_stats_files);
    nb_enc_stats_files = 0;
}

static int unescape(char **pdst, size_t *dst_len,
                    const char **pstr, char delim)
{
    const char *str = *pstr;
    char *dst;
    size_t len, idx;

    *pdst = NULL;

    len = strlen(str);
    if (!len)
        return 0;

    dst = av_malloc(len + 1);
    if (!dst)
        return AVERROR(ENOMEM);

    for (idx = 0; *str; idx++, str++) {
        if (str[0] == '\\' && str[1])
            str++;
        else if (*str == delim)
            break;

        dst[idx] = *str;
    }
    if (!idx) {
        av_freep(&dst);
        return 0;
    }

    dst[idx] = 0;

    *pdst    = dst;
    *dst_len = idx;
    *pstr    = str;

    return 0;
}

static int enc_stats_init(OutputStream *ost, EncStats *es, int pre,
                          const char *path, const char *fmt_spec)
{
    static const struct {
        enum EncStatsType  type;
        const char        *str;
        unsigned           pre_only:1;
        unsigned           post_only:1;
        unsigned           need_input_data:1;
    } fmt_specs[] = {
        { ENC_STATS_FILE_IDX,       "fidx"                      },
        { ENC_STATS_STREAM_IDX,     "sidx"                      },
        { ENC_STATS_FRAME_NUM,      "n"                         },
        { ENC_STATS_FRAME_NUM_IN,   "ni",       0, 0, 1         },
        { ENC_STATS_TIMEBASE,       "tb"                        },
        { ENC_STATS_TIMEBASE_IN,    "tbi",      0, 0, 1         },
        { ENC_STATS_PTS,            "pts"                       },
        { ENC_STATS_PTS_TIME,       "t"                         },
        { ENC_STATS_PTS_IN,         "ptsi",     0, 0, 1         },
        { ENC_STATS_PTS_TIME_IN,    "ti",       0, 0, 1         },
        { ENC_STATS_DTS,            "dts",      0, 1            },
        { ENC_STATS_DTS_TIME,       "dt",       0, 1            },
        { ENC_STATS_SAMPLE_NUM,     "sn",       1               },
        { ENC_STATS_NB_SAMPLES,     "samp",     1               },
        { ENC_STATS_PKT_SIZE,       "size",     0, 1            },
        { ENC_STATS_BITRATE,        "br",       0, 1            },
        { ENC_STATS_AVG_BITRATE,    "abr",      0, 1            },
        { ENC_STATS_KEYFRAME,       "key",      0, 1            },
    };
    const char *next = fmt_spec;

    int ret;

    while (*next) {
        EncStatsComponent *c;
        char *val;
        size_t val_len;

        // get the sequence up until next opening brace
        ret = unescape(&val, &val_len, &next, '{');
        if (ret < 0)
            return ret;

        if (val) {
            ret = GROW_ARRAY(es->components, es->nb_components);
            if (ret < 0) {
                av_freep(&val);
                return ret;
            }

            c          = &es->components[es->nb_components - 1];
            c->type    = ENC_STATS_LITERAL;
            c->str     = val;
            c->str_len = val_len;
        }

        if (!*next)
            break;
        next++;

        // get the part inside braces
        ret = unescape(&val, &val_len, &next, '}');
        if (ret < 0)
            return ret;

        if (!val) {
            av_log(NULL, AV_LOG_ERROR,
                   "Empty formatting directive in: %s\n", fmt_spec);
            return AVERROR(EINVAL);
        }

        if (!*next) {
            av_log(NULL, AV_LOG_ERROR,
                   "Missing closing brace in: %s\n", fmt_spec);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        next++;

        ret = GROW_ARRAY(es->components, es->nb_components);
        if (ret < 0)
            goto fail;

        c = &es->components[es->nb_components - 1];

        for (size_t i = 0; i < FF_ARRAY_ELEMS(fmt_specs); i++) {
            if (!strcmp(val, fmt_specs[i].str)) {
                if ((pre && fmt_specs[i].post_only) || (!pre && fmt_specs[i].pre_only)) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Format directive '%s' may only be used %s-encoding\n",
                           val, pre ? "post" : "pre");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }

                c->type = fmt_specs[i].type;

                if (fmt_specs[i].need_input_data && !ost->ist) {
                    av_log(ost, AV_LOG_WARNING,
                           "Format directive '%s' is unavailable, because "
                           "this output stream has no associated input stream\n",
                           val);
                }

                break;
            }
        }

        if (!c->type) {
            av_log(NULL, AV_LOG_ERROR, "Invalid format directive: %s\n", val);
            ret = AVERROR(EINVAL);
            goto fail;
        }

fail:
        av_freep(&val);
        if (ret < 0)
            return ret;
    }

    ret = pthread_mutex_init(&es->lock, NULL);
    if (ret)
        return AVERROR(ret);
    es->lock_initialized = 1;

    ret = enc_stats_get_file(&es->io, path);
    if (ret < 0)
        return ret;

    return 0;
}

static const char *output_stream_item_name(void *obj)
{
    const MuxStream *ms = obj;

    return ms->log_name;
}

static const AVClass output_stream_class = {
    .class_name = "OutputStream",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = output_stream_item_name,
    .category   = AV_CLASS_CATEGORY_MUXER,
};

static MuxStream *mux_stream_alloc(Muxer *mux, enum AVMediaType type)
{
    const char *type_str = av_get_media_type_string(type);
    MuxStream *ms;

    ms = allocate_array_elem(&mux->of.streams, sizeof(*ms), &mux->of.nb_streams);
    if (!ms)
        return NULL;

    ms->ost.file       = &mux->of;
    ms->ost.index      = mux->of.nb_streams - 1;
    ms->ost.type       = type;

    ms->ost.class = &output_stream_class;

    ms->sch_idx     = -1;
    ms->sch_idx_enc = -1;

    snprintf(ms->log_name, sizeof(ms->log_name), "%cost#%d:%d",
             type_str ? *type_str : '?', mux->of.index, ms->ost.index);

    return ms;
}

static int ost_get_filters(const OptionsContext *o, AVFormatContext *oc,
                           OutputStream *ost, char **dst)
{
    const char *filters = NULL;
#if FFMPEG_OPT_FILTER_SCRIPT
    const char *filters_script = NULL;

    opt_match_per_stream_str(ost, &o->filter_scripts, oc, ost->st, &filters_script);
#endif
    opt_match_per_stream_str(ost, &o->filters, oc, ost->st, &filters);

    if (!ost->ist) {
        if (
#if FFMPEG_OPT_FILTER_SCRIPT
            filters_script ||
#endif
            filters) {
            av_log(ost, AV_LOG_ERROR,
                   "%s '%s' was specified for a stream fed from a complex "
                   "filtergraph. Simple and complex filtering cannot be used "
                   "together for the same stream.\n",
#if FFMPEG_OPT_FILTER_SCRIPT
                   filters ? "Filtergraph" : "Filtergraph script",
                   filters ? filters : filters_script
#else
                   "Filtergraph", filters
#endif
                   );
            return AVERROR(EINVAL);
        }
        return 0;
    }

#if FFMPEG_OPT_FILTER_SCRIPT
    if (filters_script && filters) {
        av_log(ost, AV_LOG_ERROR, "Both -filter and -filter_script set\n");
        return AVERROR(EINVAL);
    }

    if (filters_script)
        *dst = read_file_to_string(filters_script);
    else
#endif
    if (filters)
        *dst = av_strdup(filters);
    else
        *dst = av_strdup(ost->type == AVMEDIA_TYPE_VIDEO ? "null" : "anull");
    return *dst ? 0 : AVERROR(ENOMEM);
}

static int parse_matrix_coeffs(void *logctx, uint16_t *dest, const char *str)
{
    const char *p = str;
    for (int i = 0;; i++) {
        dest[i] = atoi(p);
        if (i == 63)
            break;
        p = strchr(p, ',');
        if (!p) {
            av_log(logctx, AV_LOG_FATAL,
                   "Syntax error in matrix \"%s\" at coeff %d\n", str, i);
            return AVERROR(EINVAL);
        }
        p++;
    }

    return 0;
}

static int pixfmt_in_list(const enum AVPixelFormat *formats, enum AVPixelFormat format)
{
    for (; *formats != AV_PIX_FMT_NONE; formats++)
        if (*formats == format)
            return 1;
    return 0;
}

static enum AVPixelFormat
choose_pixel_fmt(const AVCodecContext *avctx, enum AVPixelFormat target)
{
    const enum AVPixelFormat *p;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(target);
    //FIXME: This should check for AV_PIX_FMT_FLAG_ALPHA after PAL8 pixel format without alpha is implemented
    int has_alpha = desc ? desc->nb_components % 2 == 0 : 0;
    enum AVPixelFormat best= AV_PIX_FMT_NONE;
    int ret;

    ret = avcodec_get_supported_config(avctx, NULL, AV_CODEC_CONFIG_PIX_FORMAT,
                                       0, (const void **) &p, NULL);
    if (ret < 0)
        return AV_PIX_FMT_NONE;

    for (; *p != AV_PIX_FMT_NONE; p++) {
        best = av_find_best_pix_fmt_of_2(best, *p, target, has_alpha, NULL);
        if (*p == target)
            break;
    }
    if (*p == AV_PIX_FMT_NONE) {
        if (target != AV_PIX_FMT_NONE)
            av_log(NULL, AV_LOG_WARNING,
                   "Incompatible pixel format '%s' for codec '%s', auto-selecting format '%s'\n",
                   av_get_pix_fmt_name(target),
                   avctx->codec->name,
                   av_get_pix_fmt_name(best));
        return best;
    }
    return target;
}

static enum AVPixelFormat pix_fmt_parse(OutputStream *ost, const char *name)
{
    const enum AVPixelFormat *fmts;
    enum AVPixelFormat fmt;
    int ret;

    fmt = av_get_pix_fmt(name);
    if (fmt == AV_PIX_FMT_NONE) {
        av_log(ost, AV_LOG_FATAL, "Unknown pixel format requested: %s.\n", name);
        return AV_PIX_FMT_NONE;
    }

    ret = avcodec_get_supported_config(ost->enc->enc_ctx, NULL, AV_CODEC_CONFIG_PIX_FORMAT,
                                       0, (const void **) &fmts, NULL);
    if (ret < 0)
        return AV_PIX_FMT_NONE;

    /* when the user specified-format is an alias for an endianness-specific
     * one (e.g. rgb48 -> rgb48be/le), it gets translated into the native
     * endianness by av_get_pix_fmt();
     * the following code handles the case when the native endianness is not
     * supported by the encoder, but the other one is */
    if (fmts && !pixfmt_in_list(fmts, fmt)) {
        const char *name_canonical = av_get_pix_fmt_name(fmt);
        int len = strlen(name_canonical);

        if (strcmp(name, name_canonical) &&
            (!strcmp(name_canonical + len - 2, "le") ||
             !strcmp(name_canonical + len - 2, "be"))) {
            char name_other[64];
            enum AVPixelFormat fmt_other;

            snprintf(name_other, sizeof(name_other), "%s%ce",
                     name, name_canonical[len - 2] == 'l' ? 'b' : 'l');
            fmt_other = av_get_pix_fmt(name_other);
            if (fmt_other != AV_PIX_FMT_NONE && pixfmt_in_list(fmts, fmt_other)) {
                av_log(ost, AV_LOG_VERBOSE, "Mapping pixel format %s->%s\n",
                       name, name_other);
                fmt = fmt_other;
            }
        }
    }

    if (fmts && !pixfmt_in_list(fmts, fmt))
        fmt = choose_pixel_fmt(ost->enc->enc_ctx, fmt);

    return fmt;
}

static int new_stream_video(Muxer *mux, const OptionsContext *o,
                            OutputStream *ost, int *keep_pix_fmt,
                            enum VideoSyncMethod *vsync_method)
{
    MuxStream       *ms = ms_from_ost(ost);
    AVFormatContext *oc = mux->fc;
    AVStream *st;
    const char *frame_rate = NULL, *max_frame_rate = NULL, *frame_aspect_ratio = NULL;
    int ret = 0;

    st  = ost->st;

    opt_match_per_stream_str(ost, &o->frame_rates, oc, st, &frame_rate);
    if (frame_rate && av_parse_video_rate(&ms->frame_rate, frame_rate) < 0) {
        av_log(ost, AV_LOG_FATAL, "Invalid framerate value: %s\n", frame_rate);
        return AVERROR(EINVAL);
    }

    opt_match_per_stream_str(ost, &o->max_frame_rates, oc, st, &max_frame_rate);
    if (max_frame_rate && av_parse_video_rate(&ms->max_frame_rate, max_frame_rate) < 0) {
        av_log(ost, AV_LOG_FATAL, "Invalid maximum framerate value: %s\n", max_frame_rate);
        return AVERROR(EINVAL);
    }

    if (frame_rate && max_frame_rate) {
        av_log(ost, AV_LOG_ERROR, "Only one of -fpsmax and -r can be set for a stream.\n");
        return AVERROR(EINVAL);
    }

    opt_match_per_stream_str(ost, &o->frame_aspect_ratios, oc, st, &frame_aspect_ratio);
    if (frame_aspect_ratio) {
        AVRational q;
        if (av_parse_ratio(&q, frame_aspect_ratio, 255, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(ost, AV_LOG_FATAL, "Invalid aspect ratio: %s\n", frame_aspect_ratio);
            return AVERROR(EINVAL);
        }
        ost->frame_aspect_ratio = q;
    }

    if (ost->enc) {
        AVCodecContext *video_enc = ost->enc->enc_ctx;
        const char *p = NULL, *fps_mode = NULL;
        const char *frame_size = NULL;
        const char *frame_pix_fmt = NULL;
        const char *intra_matrix = NULL, *inter_matrix = NULL;
        const char *chroma_intra_matrix = NULL;
        int do_pass = 0;
        int i;

        opt_match_per_stream_str(ost, &o->frame_sizes, oc, st, &frame_size);
        if (frame_size) {
            ret = av_parse_video_size(&video_enc->width, &video_enc->height, frame_size);
            if (ret < 0) {
                av_log(ost, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
                return AVERROR(EINVAL);
            }
        }

        opt_match_per_stream_str(ost, &o->frame_pix_fmts, oc, st, &frame_pix_fmt);
        if (frame_pix_fmt && *frame_pix_fmt == '+') {
            *keep_pix_fmt = 1;
            if (!*++frame_pix_fmt)
                frame_pix_fmt = NULL;
        }
        if (frame_pix_fmt) {
            video_enc->pix_fmt = pix_fmt_parse(ost, frame_pix_fmt);
            if (video_enc->pix_fmt == AV_PIX_FMT_NONE)
                return AVERROR(EINVAL);
        }

        opt_match_per_stream_str(ost, &o->intra_matrices, oc, st, &intra_matrix);
        if (intra_matrix) {
            if (!(video_enc->intra_matrix = av_mallocz(sizeof(*video_enc->intra_matrix) * 64)))
                return AVERROR(ENOMEM);

            ret = parse_matrix_coeffs(ost, video_enc->intra_matrix, intra_matrix);
            if (ret < 0)
                return ret;
        }
        opt_match_per_stream_str(ost, &o->chroma_intra_matrices, oc, st, &chroma_intra_matrix);
        if (chroma_intra_matrix) {
            if (!(video_enc->chroma_intra_matrix = av_mallocz(sizeof(*video_enc->chroma_intra_matrix) * 64)))
                return AVERROR(ENOMEM);
            ret = parse_matrix_coeffs(ost, video_enc->chroma_intra_matrix, chroma_intra_matrix);
            if (ret < 0)
                return ret;
        }
        opt_match_per_stream_str(ost, &o->inter_matrices, oc, st, &inter_matrix);
        if (inter_matrix) {
            if (!(video_enc->inter_matrix = av_mallocz(sizeof(*video_enc->inter_matrix) * 64)))
                return AVERROR(ENOMEM);
            ret = parse_matrix_coeffs(ost, video_enc->inter_matrix, inter_matrix);
            if (ret < 0)
                return ret;
        }

        opt_match_per_stream_str(ost, &o->rc_overrides, oc, st, &p);
        for (i = 0; p; i++) {
            int start, end, q;
            int e = sscanf(p, "%d,%d,%d", &start, &end, &q);
            if (e != 3) {
                av_log(ost, AV_LOG_FATAL, "error parsing rc_override\n");
                return AVERROR(EINVAL);
            }
            video_enc->rc_override =
                av_realloc_array(video_enc->rc_override,
                                 i + 1, sizeof(RcOverride));
            if (!video_enc->rc_override) {
                av_log(ost, AV_LOG_FATAL, "Could not (re)allocate memory for rc_override.\n");
                return AVERROR(ENOMEM);
            }
            video_enc->rc_override[i].start_frame = start;
            video_enc->rc_override[i].end_frame   = end;
            if (q > 0) {
                video_enc->rc_override[i].qscale         = q;
                video_enc->rc_override[i].quality_factor = 1.0;
            }
            else {
                video_enc->rc_override[i].qscale         = 0;
                video_enc->rc_override[i].quality_factor = -q/100.0;
            }
            p = strchr(p, '/');
            if (p) p++;
        }
        video_enc->rc_override_count = i;

        /* two pass mode */
        opt_match_per_stream_int(ost, &o->pass, oc, st, &do_pass);
        if (do_pass) {
            if (do_pass & 1)
                video_enc->flags |= AV_CODEC_FLAG_PASS1;
            if (do_pass & 2)
                video_enc->flags |= AV_CODEC_FLAG_PASS2;
        }

        opt_match_per_stream_str(ost, &o->passlogfiles, oc, st, &ost->logfile_prefix);
        if (ost->logfile_prefix &&
            !(ost->logfile_prefix = av_strdup(ost->logfile_prefix)))
            return AVERROR(ENOMEM);

        if (do_pass) {
            int ost_idx = -1;
            char logfilename[1024];
            FILE *f;

            /* compute this stream's global index */
            for (int idx = 0; idx <= ost->file->index; idx++)
                ost_idx += output_files[idx]->nb_streams;

            snprintf(logfilename, sizeof(logfilename), "%s-%d.log",
                     ost->logfile_prefix ? ost->logfile_prefix :
                                           DEFAULT_PASS_LOGFILENAME_PREFIX,
                     ost_idx);
            if (!strcmp(video_enc->codec->name, "libx264") || !strcmp(video_enc->codec->name, "libvvenc")) {
                if (av_opt_is_set_to_default_by_name(video_enc, "stats",
                                                     AV_OPT_SEARCH_CHILDREN) > 0)
                    av_opt_set(video_enc, "stats", logfilename,
                               AV_OPT_SEARCH_CHILDREN);
            } else if (!strcmp(video_enc->codec->name, "libx265")) {
                if (av_opt_is_set_to_default_by_name(video_enc, "x265-stats",
                                                     AV_OPT_SEARCH_CHILDREN) > 0)
                    av_opt_set(video_enc, "x265-stats", logfilename,
                               AV_OPT_SEARCH_CHILDREN);
            } else {
                if (video_enc->flags & AV_CODEC_FLAG_PASS2) {
                    char  *logbuffer = read_file_to_string(logfilename);

                    if (!logbuffer) {
                        av_log(ost, AV_LOG_FATAL, "Error reading log file '%s' for pass-2 encoding\n",
                               logfilename);
                        return AVERROR(EIO);
                    }
                    video_enc->stats_in = logbuffer;
                }
                if (video_enc->flags & AV_CODEC_FLAG_PASS1) {
                    f = fopen_utf8(logfilename, "wb");
                    if (!f) {
                        av_log(ost, AV_LOG_FATAL,
                               "Cannot write log file '%s' for pass-1 encoding: %s\n",
                               logfilename, strerror(errno));
                        return AVERROR(errno);
                    }
                    ost->logfile = f;
                }
            }
        }

        opt_match_per_stream_int(ost, &o->force_fps, oc, st, &ms->force_fps);

#if FFMPEG_OPT_TOP
        ost->top_field_first = -1;
        opt_match_per_stream_int(ost, &o->top_field_first, oc, st, &ost->top_field_first);
        if (ost->top_field_first >= 0)
            av_log(ost, AV_LOG_WARNING, "-top is deprecated, use the setfield filter instead\n");
#endif

#if FFMPEG_OPT_VSYNC
        *vsync_method = video_sync_method;
#else
        *vsync_method = VSYNC_AUTO;
#endif
        opt_match_per_stream_str(ost, &o->fps_mode, oc, st, &fps_mode);
        if (fps_mode) {
            ret = parse_and_set_vsync(fps_mode, vsync_method, ost->file->index, ost->index, 0);
            if (ret < 0)
                return ret;
        }

        if ((ms->frame_rate.num || ms->max_frame_rate.num) &&
            !(*vsync_method == VSYNC_AUTO ||
              *vsync_method == VSYNC_CFR || *vsync_method == VSYNC_VSCFR)) {
            av_log(ost, AV_LOG_FATAL, "One of -r/-fpsmax was specified "
                   "together a non-CFR -vsync/-fps_mode. This is contradictory.\n");
            return AVERROR(EINVAL);
        }

        if (*vsync_method == VSYNC_AUTO) {
            if (ms->frame_rate.num || ms->max_frame_rate.num) {
                *vsync_method = VSYNC_CFR;
            } else if (!strcmp(oc->oformat->name, "avi")) {
                *vsync_method = VSYNC_VFR;
            } else {
                *vsync_method = (oc->oformat->flags & AVFMT_VARIABLE_FPS)  ?
                                ((oc->oformat->flags & AVFMT_NOTIMESTAMPS) ?
                                VSYNC_PASSTHROUGH : VSYNC_VFR) : VSYNC_CFR;
            }

            if (ost->ist && *vsync_method == VSYNC_CFR) {
                const InputFile *ifile = ost->ist->file;

                if (ifile->nb_streams == 1 && ifile->input_ts_offset == 0)
                    *vsync_method = VSYNC_VSCFR;
            }

            if (*vsync_method == VSYNC_CFR && copy_ts) {
                *vsync_method = VSYNC_VSCFR;
            }
        }
#if FFMPEG_OPT_VSYNC_DROP
        if (*vsync_method == VSYNC_DROP)
            ms->ts_drop = 1;
#endif
    }

    return 0;
}

static int new_stream_audio(Muxer *mux, const OptionsContext *o,
                            OutputStream *ost)
{
    MuxStream *ms = ms_from_ost(ost);
    AVFormatContext *oc = mux->fc;
    AVStream *st = ost->st;

    if (ost->enc) {
        AVCodecContext *audio_enc = ost->enc->enc_ctx;
        int channels = 0;
        const char *layout = NULL;
        const char *sample_fmt = NULL;

        opt_match_per_stream_int(ost, &o->audio_channels, oc, st, &channels);
        if (channels) {
            audio_enc->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
            audio_enc->ch_layout.nb_channels = channels;
        }

        opt_match_per_stream_str(ost, &o->audio_ch_layouts, oc, st, &layout);
        if (layout && av_channel_layout_from_string(&audio_enc->ch_layout, layout) < 0) {
            av_log(ost, AV_LOG_FATAL, "Unknown channel layout: %s\n", layout);
            return AVERROR(EINVAL);
        }

        opt_match_per_stream_str(ost, &o->sample_fmts, oc, st, &sample_fmt);
        if (sample_fmt &&
            (audio_enc->sample_fmt = av_get_sample_fmt(sample_fmt)) == AV_SAMPLE_FMT_NONE) {
            av_log(ost, AV_LOG_FATAL, "Invalid sample format '%s'\n", sample_fmt);
            return AVERROR(EINVAL);
        }

        opt_match_per_stream_int(ost, &o->audio_sample_rate, oc, st, &audio_enc->sample_rate);
        opt_match_per_stream_str(ost, &o->apad, oc, st, &ms->apad);
    }

    return 0;
}

static int new_stream_subtitle(Muxer *mux, const OptionsContext *o,
                               OutputStream *ost)
{
    AVStream *st;

    st  = ost->st;

    if (ost->enc) {
        AVCodecContext *subtitle_enc = ost->enc->enc_ctx;

        AVCodecDescriptor const *input_descriptor =
            avcodec_descriptor_get(ost->ist->par->codec_id);
        AVCodecDescriptor const *output_descriptor =
            avcodec_descriptor_get(subtitle_enc->codec_id);
        int input_props = 0, output_props = 0;

        const char *frame_size = NULL;

        opt_match_per_stream_str(ost, &o->frame_sizes, mux->fc, st, &frame_size);
        if (frame_size) {
            int ret = av_parse_video_size(&subtitle_enc->width, &subtitle_enc->height, frame_size);
            if (ret < 0) {
                av_log(ost, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
                return ret;
            }
        }
        if (input_descriptor)
            input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
        if (output_descriptor)
            output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
        if (input_props && output_props && input_props != output_props) {
            av_log(ost, AV_LOG_ERROR,
                   "Subtitle encoding currently only possible from text to text "
                   "or bitmap to bitmap\n");
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int
ost_bind_filter(const Muxer *mux, MuxStream *ms, OutputFilter *ofilter,
                const OptionsContext *o,
                AVRational enc_tb, enum VideoSyncMethod vsync_method,
                int keep_pix_fmt, int autoscale, int threads_manual,
                const ViewSpecifier *vs,
                SchedulerNode *src)
{
    OutputStream       *ost = &ms->ost;
    AVCodecContext *enc_ctx = ost->enc->enc_ctx;
    char name[16];
    char *filters = NULL;
    int ret;

    OutputFilterOptions opts = {
        .enc              = enc_ctx->codec,
        .name             = name,
        .format           = (ost->type == AVMEDIA_TYPE_VIDEO) ?
                            enc_ctx->pix_fmt : enc_ctx->sample_fmt,
        .width            = enc_ctx->width,
        .height           = enc_ctx->height,
        .color_space      = enc_ctx->colorspace,
        .color_range      = enc_ctx->color_range,
        .alpha_mode       = enc_ctx->alpha_mode,
        .vsync_method     = vsync_method,
        .frame_rate       = ms->frame_rate,
        .max_frame_rate   = ms->max_frame_rate,
        .sample_rate      = enc_ctx->sample_rate,
        .ch_layout        = enc_ctx->ch_layout,
        .sws_opts         = o->g->sws_dict,
        .swr_opts         = o->g->swr_opts,
        .output_tb        = enc_tb,
        .trim_start_us    = mux->of.start_time,
        .trim_duration_us = mux->of.recording_time,
        .ts_offset        = mux->of.start_time == AV_NOPTS_VALUE ?
                            0 : mux->of.start_time,
        .vs               = vs,
        .nb_threads       = -1,

        .flags = OFILTER_FLAG_DISABLE_CONVERT * !!keep_pix_fmt |
                 OFILTER_FLAG_AUTOSCALE       * !!autoscale    |
                 OFILTER_FLAG_AUDIO_24BIT * !!(av_get_exact_bits_per_sample(enc_ctx->codec_id) == 24),
    };

    snprintf(name, sizeof(name), "#%d:%d", mux->of.index, ost->index);

    if (ost->type == AVMEDIA_TYPE_VIDEO) {
        if (!keep_pix_fmt) {
            ret = avcodec_get_supported_config(enc_ctx, NULL,
                                               AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                               (const void **) &opts.pix_fmts, NULL);
            if (ret < 0)
                return ret;
        }
        if (!ms->force_fps) {
            ret = avcodec_get_supported_config(enc_ctx, NULL,
                                               AV_CODEC_CONFIG_FRAME_RATE, 0,
                                               (const void **) &opts.frame_rates, NULL);
            if (ret < 0)
                return ret;
        }
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_COLOR_SPACE, 0,
                                           (const void **) &opts.color_spaces, NULL);
        if (ret < 0)
            return ret;
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_COLOR_RANGE, 0,
                                           (const void **) &opts.color_ranges, NULL);
        if (ret < 0)
            return ret;
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_ALPHA_MODE, 0,
                                           (const void **) &opts.alpha_modes, NULL);
        if (ret < 0)
            return ret;
    } else {
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                           (const void **) &opts.sample_fmts, NULL);
        if (ret < 0)
            return ret;
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                           (const void **) &opts.sample_rates, NULL);
        if (ret < 0)
            return ret;
        ret = avcodec_get_supported_config(enc_ctx, NULL,
                                           AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
                                           (const void **) &opts.ch_layouts, NULL);
        if (ret < 0)
            return ret;
    }

    if (threads_manual) {
        ret = av_opt_get_int(enc_ctx, "threads", 0, &opts.nb_threads);
        if (ret < 0)
            return ret;
    }

    ret = ost_get_filters(o, mux->fc, ost, &filters);
    if (ret < 0)
        return ret;

    if (ofilter) {
        av_assert0(!filters);
        ost->filter = ofilter;
        ret = ofilter_bind_enc(ofilter, ms->sch_idx_enc, &opts);
    } else {
        ret = fg_create_simple(&ost->fg_simple, ost->ist, &filters,
                               mux->sch, ms->sch_idx_enc, &opts);
        if (ret >= 0)
            ost->filter = ost->fg_simple->outputs[0];

    }
    if (ret < 0)
        return ret;

    *src = SCH_ENC(ms->sch_idx_enc);

    return 0;
}

static int streamcopy_init(const OptionsContext *o, const Muxer *mux,
                           OutputStream *ost, AVDictionary **encoder_opts)
{
    MuxStream           *ms         = ms_from_ost(ost);

    const InputStream   *ist        = ost->ist;
    const InputFile     *ifile      = ist->file;

    AVCodecParameters   *par        = ms->par_in;
    uint32_t             codec_tag  = par->codec_tag;

    AVCodecContext      *codec_ctx  = NULL;

    AVRational           fr         = ms->frame_rate;

    int ret = 0;

    const char *filters = NULL;
#if FFMPEG_OPT_FILTER_SCRIPT
    const char *filters_script = NULL;

    opt_match_per_stream_str(ost, &o->filter_scripts, mux->fc, ost->st, &filters_script);
#endif
    opt_match_per_stream_str(ost, &o->filters, mux->fc, ost->st, &filters);

    if (
#if FFMPEG_OPT_FILTER_SCRIPT
        filters_script ||
#endif
        filters) {
        av_log(ost, AV_LOG_ERROR,
               "%s '%s' was specified, but codec copy was selected. "
               "Filtering and streamcopy cannot be used together.\n",
#if FFMPEG_OPT_FILTER_SCRIPT
               filters ? "Filtergraph" : "Filtergraph script",
               filters ? filters : filters_script
#else
               "Filtergraph", filters
#endif
               );
        return AVERROR(EINVAL);
    }

    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(codec_ctx, ist->par);
    if (ret >= 0)
        ret = av_opt_set_dict(codec_ctx, encoder_opts);
    if (ret < 0) {
        av_log(ost, AV_LOG_FATAL,
               "Error setting up codec context options.\n");
        goto fail;
    }

    ret = avcodec_parameters_from_context(par, codec_ctx);
    if (ret < 0) {
        av_log(ost, AV_LOG_FATAL,
               "Error getting reference codec parameters.\n");
        goto fail;
    }

    if (!codec_tag) {
        const struct AVCodecTag * const *ct = mux->fc->oformat->codec_tag;
        unsigned int codec_tag_tmp;
        if (!ct || av_codec_get_id (ct, par->codec_tag) == par->codec_id ||
            !av_codec_get_tag2(ct, par->codec_id, &codec_tag_tmp))
            codec_tag = par->codec_tag;
    }

    par->codec_tag = codec_tag;

    if (!fr.num)
        fr = ist->framerate;

    if (fr.num)
        ost->st->avg_frame_rate = fr;
    else
        ost->st->avg_frame_rate = ist->st->avg_frame_rate;

    // copy timebase while removing common factors
    if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0) {
        if (fr.num)
            ost->st->time_base = av_inv_q(fr);
        else
            ost->st->time_base = av_add_q(ist->st->time_base, (AVRational){0, 1});
    }

    if (!ms->copy_prior_start) {
        ms->ts_copy_start = (mux->of.start_time == AV_NOPTS_VALUE) ?
                            0 : mux->of.start_time;
        if (copy_ts && ifile->start_time != AV_NOPTS_VALUE) {
            ms->ts_copy_start = FFMAX(ms->ts_copy_start,
                                      ifile->start_time + ifile->ts_offset);
        }
    }

    for (int i = 0; i < ist->st->codecpar->nb_coded_side_data; i++) {
        const AVPacketSideData *sd_src = &ist->st->codecpar->coded_side_data[i];
        AVPacketSideData *sd_dst;

        sd_dst = av_packet_side_data_new(&ost->st->codecpar->coded_side_data,
                                         &ost->st->codecpar->nb_coded_side_data,
                                         sd_src->type, sd_src->size, 0);
        if (!sd_dst) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        memcpy(sd_dst->data, sd_src->data, sd_src->size);
    }

    switch (par->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        if ((par->block_align == 1 || par->block_align == 1152 || par->block_align == 576) &&
            par->codec_id == AV_CODEC_ID_MP3)
            par->block_align = 0;
        if (par->codec_id == AV_CODEC_ID_AC3)
            par->block_align = 0;
        break;
    case AVMEDIA_TYPE_VIDEO: {
        AVRational sar;
        if (ost->frame_aspect_ratio.num) { // overridden by the -aspect cli option
            sar =
                av_mul_q(ost->frame_aspect_ratio,
                         (AVRational){ par->height, par->width });
            av_log(ost, AV_LOG_WARNING, "Overriding aspect ratio "
                   "with stream copy may produce invalid files\n");
            }
        else if (ist->st->sample_aspect_ratio.num)
            sar = ist->st->sample_aspect_ratio;
        else
            sar = par->sample_aspect_ratio;
        ost->st->sample_aspect_ratio = par->sample_aspect_ratio = sar;
        ost->st->r_frame_rate = ist->st->r_frame_rate;
        break;
        }
    }

fail:
    avcodec_free_context(&codec_ctx);
    return ret;
}

static int set_encoder_id(OutputStream *ost, const AVCodec *codec)
{
    const char *cname = codec->name;
    uint8_t *encoder_string;
    int encoder_string_len;

    encoder_string_len = sizeof(LIBAVCODEC_IDENT) + strlen(cname) + 2;
    encoder_string     = av_mallocz(encoder_string_len);
    if (!encoder_string)
        return AVERROR(ENOMEM);

    if (!ost->file->bitexact && !ost->bitexact)
        av_strlcpy(encoder_string, LIBAVCODEC_IDENT " ", encoder_string_len);
    else
        av_strlcpy(encoder_string, "Lavc ", encoder_string_len);
    av_strlcat(encoder_string, cname, encoder_string_len);
    av_dict_set(&ost->st->metadata, "encoder",  encoder_string,
                AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);

    return 0;
}

static int ost_add(Muxer *mux, const OptionsContext *o, enum AVMediaType type,
                   InputStream *ist, OutputFilter *ofilter, const ViewSpecifier *vs,
                   OutputStream **post)
{
    AVFormatContext *oc = mux->fc;
    MuxStream     *ms;
    OutputStream *ost;
    const AVCodec *enc;
    AVStream *st;
    SchedulerNode src = { .type = SCH_NODE_TYPE_NONE };
    AVDictionary *encoder_opts = NULL;
    int ret = 0, keep_pix_fmt = 0, autoscale = 1;
    int threads_manual = 0;
    AVRational enc_tb = { 0, 0 };
    enum VideoSyncMethod vsync_method = VSYNC_AUTO;
    const char *bsfs = NULL, *time_base = NULL, *codec_tag = NULL;
    char  *next;
    double qscale = -1;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    ms  = mux_stream_alloc(mux, type);
    if (!ms)
        return AVERROR(ENOMEM);

    // only streams with sources (i.e. not attachments)
    // are handled by the scheduler
    if (ist || ofilter) {
        ret = GROW_ARRAY(mux->sch_stream_idx, mux->nb_sch_stream_idx);
        if (ret < 0)
            return ret;

        ret = sch_add_mux_stream(mux->sch, mux->sch_idx);
        if (ret < 0)
            return ret;

        av_assert0(ret == mux->nb_sch_stream_idx - 1);
        mux->sch_stream_idx[ret] = ms->ost.index;
        ms->sch_idx              = ret;
    }

    ost = &ms->ost;

    if (o->streamid) {
        AVDictionaryEntry *e;
        char idx[16], *p;
        snprintf(idx, sizeof(idx), "%d", ost->index);

        e = av_dict_get(o->streamid, idx, NULL, 0);
        if (e) {
            st->id = strtol(e->value, &p, 0);
            if (!e->value[0] || *p) {
                av_log(ost, AV_LOG_FATAL, "Invalid stream id: %s\n", e->value);
                return AVERROR(EINVAL);
            }
        }
    }

    ms->par_in = avcodec_parameters_alloc();
    if (!ms->par_in)
        return AVERROR(ENOMEM);

    ms->last_mux_dts = AV_NOPTS_VALUE;

    ost->st         = st;
    ost->ist        = ist;
    ost->kf.ref_pts = AV_NOPTS_VALUE;
    ms->par_in->codec_type   = type;
    st->codecpar->codec_type = type;

    ret = choose_encoder(o, oc, ms, &enc);
    if (ret < 0) {
        av_log(ost, AV_LOG_FATAL, "Error selecting an encoder\n");
        return ret;
    }

    if (enc) {
        ret = sch_add_enc(mux->sch, encoder_thread, ost,
                          ost->type == AVMEDIA_TYPE_SUBTITLE ? NULL : enc_open);
        if (ret < 0)
            return ret;
        ms->sch_idx_enc = ret;

        ret = enc_alloc(&ost->enc, enc, mux->sch, ms->sch_idx_enc, ost);
        if (ret < 0)
            return ret;

        av_strlcat(ms->log_name, "/",       sizeof(ms->log_name));
        av_strlcat(ms->log_name, enc->name, sizeof(ms->log_name));
    } else {
        if (ofilter) {
            av_log(ost, AV_LOG_ERROR,
                   "Streamcopy requested for output stream fed "
                   "from a complex filtergraph. Filtering and streamcopy "
                   "cannot be used together.\n");
            return AVERROR(EINVAL);
        }

        av_strlcat(ms->log_name, "/copy", sizeof(ms->log_name));
    }

    av_log(ost, AV_LOG_VERBOSE, "Created %s stream from ",
           av_get_media_type_string(type));
    if (ist)
        av_log(ost, AV_LOG_VERBOSE, "input stream %d:%d",
               ist->file->index, ist->index);
    else if (ofilter)
        av_log(ost, AV_LOG_VERBOSE, "complex filtergraph %d:[%s]\n",
               ofilter->graph->index, ofilter->name);
    else if (type == AVMEDIA_TYPE_ATTACHMENT)
        av_log(ost, AV_LOG_VERBOSE, "attached file");
    else av_assert0(0);
    av_log(ost, AV_LOG_VERBOSE, "\n");

    ms->pkt = av_packet_alloc();
    if (!ms->pkt)
        return AVERROR(ENOMEM);

    if (ost->enc) {
        AVIOContext *s = NULL;
        char *buf = NULL, *arg = NULL;
        const char *enc_stats_pre = NULL, *enc_stats_post = NULL, *mux_stats = NULL;
        const char *enc_time_base = NULL, *preset = NULL;

        ret = filter_codec_opts(o->g->codec_opts, enc->id,
                                oc, st, enc, &encoder_opts,
                                &mux->enc_opts_used);
        if (ret < 0)
            goto fail;

        opt_match_per_stream_str(ost, &o->presets, oc, st, &preset);
        opt_match_per_stream_int(ost, &o->autoscale, oc, st, &autoscale);
        if (preset && (!(ret = get_preset_file_2(preset, enc->name, &s)))) {
            AVBPrint bprint;
            av_bprint_init(&bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
            do  {
                av_bprint_clear(&bprint);
                buf = get_line(s, &bprint);
                if (!buf) {
                    ret = AVERROR(ENOMEM);
                    break;
                }

                if (!buf[0] || buf[0] == '#')
                    continue;
                if (!(arg = strchr(buf, '='))) {
                    av_log(ost, AV_LOG_FATAL, "Invalid line found in the preset file.\n");
                    ret = AVERROR(EINVAL);
                    break;
                }
                *arg++ = 0;
                av_dict_set(&encoder_opts, buf, arg, AV_DICT_DONT_OVERWRITE);
            } while (!s->eof_reached);
            av_bprint_finalize(&bprint, NULL);
            avio_closep(&s);
        }
        if (ret) {
            av_log(ost, AV_LOG_FATAL,
                   "Preset %s specified, but could not be opened.\n", preset);
            goto fail;
        }

        opt_match_per_stream_str(ost, &o->enc_stats_pre, oc, st, &enc_stats_pre);
        if (enc_stats_pre &&
            (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)) {
            const char *format = "{fidx} {sidx} {n} {t}";

            opt_match_per_stream_str(ost, &o->enc_stats_pre_fmt, oc, st, &format);

            ret = enc_stats_init(ost, &ost->enc_stats_pre, 1, enc_stats_pre, format);
            if (ret < 0)
                goto fail;
        }

        opt_match_per_stream_str(ost, &o->enc_stats_post, oc, st, &enc_stats_post);
        if (enc_stats_post &&
            (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)) {
            const char *format = "{fidx} {sidx} {n} {t}";

            opt_match_per_stream_str(ost, &o->enc_stats_post_fmt, oc, st, &format);

            ret = enc_stats_init(ost, &ost->enc_stats_post, 0, enc_stats_post, format);
            if (ret < 0)
                goto fail;
        }

        opt_match_per_stream_str(ost, &o->mux_stats, oc, st, &mux_stats);
        if (mux_stats &&
            (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)) {
            const char *format = "{fidx} {sidx} {n} {t}";

            opt_match_per_stream_str(ost, &o->mux_stats_fmt, oc, st, &format);

            ret = enc_stats_init(ost, &ms->stats, 0, mux_stats, format);
            if (ret < 0)
                goto fail;
        }

        opt_match_per_stream_str(ost, &o->enc_time_bases, oc, st, &enc_time_base);
        if (enc_time_base && type == AVMEDIA_TYPE_SUBTITLE)
            av_log(ost, AV_LOG_WARNING,
                   "-enc_time_base not supported for subtitles, ignoring\n");
        else if (enc_time_base) {
            AVRational q;

            if (!strcmp(enc_time_base, "demux")) {
                q = (AVRational){ ENC_TIME_BASE_DEMUX, 0 };
            } else if (!strcmp(enc_time_base, "filter")) {
                q = (AVRational){ ENC_TIME_BASE_FILTER, 0 };
            } else {
                ret = av_parse_ratio(&q, enc_time_base, INT_MAX, 0, NULL);
                if (ret < 0 || q.den <= 0
#if !FFMPEG_OPT_ENC_TIME_BASE_NUM
                    || q.num < 0
#endif
                    ) {
                    av_log(ost, AV_LOG_FATAL, "Invalid time base: %s\n", enc_time_base);
                    ret = ret < 0 ? ret : AVERROR(EINVAL);
                    goto fail;
                }
#if FFMPEG_OPT_ENC_TIME_BASE_NUM
                if (q.num < 0)
                    av_log(ost, AV_LOG_WARNING, "-enc_time_base -1 is deprecated,"
                           " use -enc_time_base demux\n");
#endif
            }

            enc_tb = q;
        }

        threads_manual = !!av_dict_get(encoder_opts, "threads", NULL, 0);

        ret = av_opt_set_dict2(ost->enc->enc_ctx, &encoder_opts, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(ost, AV_LOG_ERROR, "Error applying encoder options: %s\n",
                   av_err2str(ret));
            goto fail;
        }

        ret = check_avoptions(encoder_opts);
        if (ret < 0)
            goto fail;

        // default to automatic thread count
        if (!threads_manual)
            ost->enc->enc_ctx->thread_count = 0;
    } else {
        ret = filter_codec_opts(o->g->codec_opts, AV_CODEC_ID_NONE, oc, st,
                                NULL, &encoder_opts,
                                &mux->enc_opts_used);
        if (ret < 0)
            goto fail;
    }


    if (o->bitexact) {
        ost->bitexact        = 1;
    } else if (ost->enc) {
        ost->bitexact        = !!(ost->enc->enc_ctx->flags & AV_CODEC_FLAG_BITEXACT);
    }

    if (enc) {
        ret = set_encoder_id(ost, enc);
        if (ret < 0)
            return ret;
    }

    opt_match_per_stream_str(ost, &o->time_bases, oc, st, &time_base);
    if (time_base) {
        AVRational q;
        if (av_parse_ratio(&q, time_base, INT_MAX, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(ost, AV_LOG_FATAL, "Invalid time base: %s\n", time_base);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        st->time_base = q;
    }

    ms->max_frames = INT64_MAX;
    opt_match_per_stream_int64(ost, &o->max_frames, oc, st, &ms->max_frames);
    for (int i = 0; i < o->max_frames.nb_opt; i++) {
        char *p = o->max_frames.opt[i].specifier;
        if (!*p && type != AVMEDIA_TYPE_VIDEO) {
            av_log(ost, AV_LOG_WARNING, "Applying unspecific -frames to non video streams, maybe you meant -vframes ?\n");
            break;
        }
    }

    ms->copy_prior_start = -1;
    opt_match_per_stream_int(ost, &o->copy_prior_start, oc, st, &ms->copy_prior_start);
    opt_match_per_stream_str(ost, &o->bitstream_filters, oc, st, &bsfs);
    if (bsfs && *bsfs) {
        ret = av_bsf_list_parse_str(bsfs, &ms->bsf_ctx);
        if (ret < 0) {
            av_log(ost, AV_LOG_ERROR, "Error parsing bitstream filter sequence '%s': %s\n", bsfs, av_err2str(ret));
            goto fail;
        }
    }

    opt_match_per_stream_str(ost, &o->codec_tags, oc, st, &codec_tag);
    if (codec_tag) {
        uint32_t tag = strtol(codec_tag, &next, 0);
        if (*next) {
            uint8_t buf[4] = { 0 };
            memcpy(buf, codec_tag, FFMIN(sizeof(buf), strlen(codec_tag)));
            tag = AV_RL32(buf);
        }
        ost->st->codecpar->codec_tag = tag;
        ms->par_in->codec_tag = tag;
        if (ost->enc)
            ost->enc->enc_ctx->codec_tag = tag;
    }

    opt_match_per_stream_dbl(ost, &o->qscale, oc, st, &qscale);
    if (ost->enc && qscale >= 0) {
        ost->enc->enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ost->enc->enc_ctx->global_quality = FF_QP2LAMBDA * qscale;
    }

    if (ms->sch_idx >= 0) {
        int max_muxing_queue_size       = 128;
        int muxing_queue_data_threshold = 50 * 1024 * 1024;

        opt_match_per_stream_int(ost, &o->max_muxing_queue_size, oc, st,
                                 &max_muxing_queue_size);
        opt_match_per_stream_int(ost, &o->muxing_queue_data_threshold,
                                 oc, st, &muxing_queue_data_threshold);

        sch_mux_stream_buffering(mux->sch, mux->sch_idx, ms->sch_idx,
                                 max_muxing_queue_size, muxing_queue_data_threshold);
    }

    opt_match_per_stream_int(ost, &o->bits_per_raw_sample, oc, st,
                                   &ost->bits_per_raw_sample);

    opt_match_per_stream_int(ost, &o->fix_sub_duration_heartbeat,
                             oc, st, &ost->fix_sub_duration_heartbeat);

    if (oc->oformat->flags & AVFMT_GLOBALHEADER && ost->enc)
        ost->enc->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    opt_match_per_stream_int(ost, &o->copy_initial_nonkeyframes,
                             oc, st, &ms->copy_initial_nonkeyframes);
    switch (type) {
    case AVMEDIA_TYPE_VIDEO:      ret = new_stream_video     (mux, o, ost, &keep_pix_fmt, &vsync_method); break;
    case AVMEDIA_TYPE_AUDIO:      ret = new_stream_audio     (mux, o, ost); break;
    case AVMEDIA_TYPE_SUBTITLE:   ret = new_stream_subtitle  (mux, o, ost); break;
    }
    if (ret < 0)
        goto fail;

    if (ost->enc &&
        (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)) {
        ret = ost_bind_filter(mux, ms, ofilter, o, enc_tb, vsync_method,
                              keep_pix_fmt, autoscale, threads_manual, vs, &src);
        if (ret < 0)
            goto fail;
    } else if (ost->ist) {
        ret = ist_use(ost->ist, !!ost->enc, NULL, &src);
        if (ret < 0) {
            av_log(ost, AV_LOG_ERROR,
                   "Error binding an input stream\n");
            goto fail;
        }
        ms->sch_idx_src = src.idx;

        // src refers to a decoder for transcoding, demux stream otherwise
        if (ost->enc) {
            ret = sch_connect(mux->sch,
                              src, SCH_ENC(ms->sch_idx_enc));
            if (ret < 0)
                goto fail;
            src = SCH_ENC(ms->sch_idx_enc);
        }
    }

    if (src.type != SCH_NODE_TYPE_NONE) {
        ret = sch_connect(mux->sch,
                          src, SCH_MSTREAM(mux->sch_idx, ms->sch_idx));
        if (ret < 0)
            goto fail;
    } else {
        // only attachment streams don't have a source
        av_assert0(type == AVMEDIA_TYPE_ATTACHMENT && ms->sch_idx < 0);
    }

    if (ost->ist && !ost->enc) {
        ret = streamcopy_init(o, mux, ost, &encoder_opts);
        if (ret < 0)
            goto fail;
    }

    // copy estimated duration as a hint to the muxer
    if (ost->ist && ost->ist->st->duration > 0) {
        ms->stream_duration    = ist->st->duration;
        ms->stream_duration_tb = ist->st->time_base;
    }

    if (post)
        *post = ost;

    ret = 0;

fail:
    av_dict_free(&encoder_opts);

    return ret;
}

static int map_auto_video(Muxer *mux, const OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    InputStreamGroup *best_istg = NULL;
    InputStream *best_ist = NULL;
    int64_t best_score = 0;
    int qcr;

    /* video: highest resolution */
    if (av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_VIDEO) == AV_CODEC_ID_NONE)
        return 0;

    qcr = avformat_query_codec(oc->oformat, oc->oformat->video_codec, 0);
    for (int j = 0; j < nb_input_files; j++) {
        InputFile *ifile = input_files[j];
        InputStreamGroup *file_best_istg = NULL;
        InputStream *file_best_ist = NULL;
        int64_t file_best_score = 0;
        for (int i = 0; i < ifile->nb_stream_groups; i++) {
            InputStreamGroup *istg = ifile->stream_groups[i];
            int64_t score = 0;

            if (!istg->fg)
                continue;

            for (int j = 0; j < istg->stg->nb_streams; j++) {
                AVStream *st = istg->stg->streams[j];

                if (st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS) {
                    score = 100000000;
                    break;
                }
            }

            switch (istg->stg->type) {
            case AV_STREAM_GROUP_PARAMS_TILE_GRID: {
                const AVStreamGroupTileGrid *tg = istg->stg->params.tile_grid;
                score += tg->width * (int64_t)tg->height
                           + 5000000*!!(istg->stg->disposition & AV_DISPOSITION_DEFAULT);
                break;
            }
            default:
                continue;
            }

            if (score > file_best_score) {
                file_best_score = score;
                file_best_istg  = istg;
            }
        }
        for (int i = 0; i < ifile->nb_streams; i++) {
            InputStream *ist = ifile->streams[i];
            const AVCodecDescriptor *desc = avcodec_descriptor_get(ist->st->codecpar->codec_id);
            int64_t score;

            if (ist->user_set_discard == AVDISCARD_ALL ||
                ist->st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                (desc && (desc->props & AV_CODEC_PROP_ENHANCEMENT)))
                continue;

            score = ist->st->codecpar->width * (int64_t)ist->st->codecpar->height
                       + 100000000 * !!(ist->st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS)
                       + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if((qcr!=MKTAG('A', 'P', 'I', 'C')) && (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                score = 1;

            if (score > file_best_score) {
                if((qcr==MKTAG('A', 'P', 'I', 'C')) && !(ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                    continue;
                file_best_score = score;
                file_best_ist   = ist;
                file_best_istg  = NULL;
            }
        }
        if (file_best_istg) {
            file_best_score -= 5000000*!!(file_best_istg->stg->disposition & AV_DISPOSITION_DEFAULT);
            if (file_best_score > best_score) {
                best_score = file_best_score;
                best_istg = file_best_istg;
                best_ist = NULL;
            }
        }
        if (file_best_ist) {
            if((qcr == MKTAG('A', 'P', 'I', 'C')) ||
               !(file_best_ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                file_best_score -= 5000000*!!(file_best_ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if (file_best_score > best_score) {
                best_score = file_best_score;
                best_ist = file_best_ist;
                best_istg = NULL;
            }
       }
    }
    if (best_istg) {
        FilterGraph *fg = best_istg->fg;
        OutputFilter *ofilter = fg->outputs[0];

        av_assert0(fg->nb_outputs == 1);
        av_log(mux, AV_LOG_VERBOSE, "Creating output stream from stream group derived complex filtergraph %d.\n", fg->index);

        return ost_add(mux, o, AVMEDIA_TYPE_VIDEO, NULL, ofilter, NULL, NULL);
    }
    if (best_ist)
        return ost_add(mux, o, AVMEDIA_TYPE_VIDEO, best_ist, NULL, NULL, NULL);

    return 0;
}

static int map_auto_audio(Muxer *mux, const OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    InputStream *best_ist = NULL;
    int best_score = 0;

        /* audio: most channels */
    if (av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_AUDIO) == AV_CODEC_ID_NONE)
        return 0;

    for (int j = 0; j < nb_input_files; j++) {
        InputFile *ifile = input_files[j];
        InputStream *file_best_ist = NULL;
        int file_best_score = 0;
        for (int i = 0; i < ifile->nb_streams; i++) {
            InputStream *ist = ifile->streams[i];
            int score;

            if (ist->user_set_discard == AVDISCARD_ALL ||
                ist->st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;

            score = ist->st->codecpar->ch_layout.nb_channels
                    + 100000000 * !!(ist->st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS)
                    + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if (score > file_best_score) {
                file_best_score = score;
                file_best_ist   = ist;
            }
        }
        if (file_best_ist) {
            file_best_score -= 5000000*!!(file_best_ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if (file_best_score > best_score) {
                best_score = file_best_score;
                best_ist   = file_best_ist;
            }
       }
    }
    if (best_ist)
        return ost_add(mux, o, AVMEDIA_TYPE_AUDIO, best_ist, NULL, NULL, NULL);

    return 0;
}

static int map_auto_subtitle(Muxer *mux, const OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    const char *subtitle_codec_name = NULL;

        /* subtitles: pick first */
    subtitle_codec_name = opt_match_per_type_str(&o->codec_names, 's');
    if (!avcodec_find_encoder(oc->oformat->subtitle_codec) && !subtitle_codec_name)
        return 0;

    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist))
        if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            AVCodecDescriptor const *input_descriptor =
                avcodec_descriptor_get(ist->st->codecpar->codec_id);
            AVCodecDescriptor const *output_descriptor = NULL;
            AVCodec const *output_codec =
                avcodec_find_encoder(oc->oformat->subtitle_codec);
            int input_props = 0, output_props = 0;
            if (ist->user_set_discard == AVDISCARD_ALL)
                continue;
            if (output_codec)
                output_descriptor = avcodec_descriptor_get(output_codec->id);
            if (input_descriptor)
                input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (output_descriptor)
                output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (subtitle_codec_name ||
                input_props & output_props ||
                // Map dvb teletext which has neither property to any output subtitle encoder
                input_descriptor && output_descriptor &&
                (!input_descriptor->props ||
                 !output_descriptor->props)) {
                return ost_add(mux, o, AVMEDIA_TYPE_SUBTITLE, ist, NULL, NULL, NULL);
            }
        }

    return 0;
}

static int map_auto_data(Muxer *mux, const OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    /* Data only if codec id match */
    enum AVCodecID codec_id = av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_DATA);

    if (codec_id == AV_CODEC_ID_NONE)
        return 0;

    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        if (ist->user_set_discard == AVDISCARD_ALL)
            continue;
        if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA &&
            ist->st->codecpar->codec_id == codec_id) {
            int ret = ost_add(mux, o, AVMEDIA_TYPE_DATA, ist, NULL, NULL, NULL);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int map_manual(Muxer *mux, const OptionsContext *o, const StreamMap *map)
{
    InputStream *ist;
    int ret;

    if (map->disabled)
        return 0;

    if (map->linklabel) {
        FilterGraph *fg;
        OutputFilter *ofilter = NULL;
        int j, k;

        for (j = 0; j < nb_filtergraphs; j++) {
            fg = filtergraphs[j];
            for (k = 0; k < fg->nb_outputs; k++) {
                const char *linklabel = fg->outputs[k]->linklabel;
                if (linklabel && !strcmp(linklabel, map->linklabel)) {
                    ofilter = fg->outputs[k];
                    goto loop_end;
                }
            }
        }
loop_end:
        if (!ofilter) {
            av_log(mux, AV_LOG_FATAL, "Output with label '%s' does not exist "
                   "in any defined filter graph, or was already used elsewhere.\n", map->linklabel);
            return AVERROR(EINVAL);
        }

        av_log(mux, AV_LOG_VERBOSE, "Creating output stream from an explicitly "
               "mapped complex filtergraph %d, output [%s]\n", fg->index, map->linklabel);

        ret = ost_add(mux, o, ofilter->type, NULL, ofilter, NULL, NULL);
        if (ret < 0)
            return ret;
    } else {
        const ViewSpecifier *vs = map->vs.type == VIEW_SPECIFIER_TYPE_NONE ?
                                  NULL : &map->vs;

        ist = input_files[map->file_index]->streams[map->stream_index];
        if (ist->user_set_discard == AVDISCARD_ALL) {
            av_log(mux, AV_LOG_FATAL, "Stream #%d:%d is disabled and cannot be mapped.\n",
                   map->file_index, map->stream_index);
            return AVERROR(EINVAL);
        }
        if(o->subtitle_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            return 0;
        if(o->   audio_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            return 0;
        if(o->   video_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            return 0;
        if(o->    data_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
            return 0;

        if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_UNKNOWN &&
            !copy_unknown_streams) {
            av_log(mux, ignore_unknown_streams ? AV_LOG_WARNING : AV_LOG_FATAL,
                   "Cannot map stream #%d:%d - unsupported type.\n",
                   map->file_index, map->stream_index);
            if (!ignore_unknown_streams) {
                av_log(mux, AV_LOG_FATAL,
                       "If you want unsupported types ignored instead "
                       "of failing, please use the -ignore_unknown option\n"
                       "If you want them copied, please use -copy_unknown\n");
                return AVERROR(EINVAL);
            }
            return 0;
        }

        if (vs && ist->st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            av_log(mux, AV_LOG_ERROR,
                   "View specifier given for mapping a %s input stream\n",
                   av_get_media_type_string(ist->st->codecpar->codec_type));
            return AVERROR(EINVAL);
        }

        ret = ost_add(mux, o, ist->st->codecpar->codec_type, ist, NULL, vs, NULL);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int of_add_attachments(Muxer *mux, const OptionsContext *o)
{
    MuxStream *ms;
    OutputStream *ost;
    int err;

    for (int i = 0; i < o->nb_attachments; i++) {
        AVIOContext *pb;
        uint8_t *attachment;
        char *attachment_filename;
        const char *p;
        int64_t len;

        if ((err = avio_open2(&pb, o->attachments[i], AVIO_FLAG_READ, &int_cb, NULL)) < 0) {
            av_log(mux, AV_LOG_FATAL, "Could not open attachment file %s.\n",
                   o->attachments[i]);
            return err;
        }
        if ((len = avio_size(pb)) <= 0) {
            av_log(mux, AV_LOG_FATAL, "Could not get size of the attachment %s.\n",
                   o->attachments[i]);
            err = len ? len : AVERROR_INVALIDDATA;
            goto read_fail;
        }
        if (len > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
            av_log(mux, AV_LOG_FATAL, "Attachment %s too large.\n",
                   o->attachments[i]);
            err = AVERROR(ERANGE);
            goto read_fail;
        }

        attachment = av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!attachment) {
            err = AVERROR(ENOMEM);
            goto read_fail;
        }

        err = avio_read(pb, attachment, len);
        if (err < 0)
            av_log(mux, AV_LOG_FATAL, "Error reading attachment file %s: %s\n",
                   o->attachments[i], av_err2str(err));
        else if (err != len) {
            av_log(mux, AV_LOG_FATAL, "Could not read all %"PRId64" bytes for "
                   "attachment file %s\n", len, o->attachments[i]);
            err = AVERROR(EIO);
        }

read_fail:
        avio_closep(&pb);
        if (err < 0)
            return err;

        memset(attachment + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        av_log(mux, AV_LOG_VERBOSE, "Creating attachment stream from file %s\n",
               o->attachments[i]);

        attachment_filename = av_strdup(o->attachments[i]);
        if (!attachment_filename) {
            av_free(attachment);
            return AVERROR(ENOMEM);
        }

        err = ost_add(mux, o, AVMEDIA_TYPE_ATTACHMENT, NULL, NULL, NULL, &ost);
        if (err < 0) {
            av_free(attachment_filename);
            av_freep(&attachment);
            return err;
        }

        ms = ms_from_ost(ost);

        ost->attachment_filename       = attachment_filename;
        ms->par_in->extradata          = attachment;
        ms->par_in->extradata_size     = len;

        p = strrchr(o->attachments[i], '/');
        av_dict_set(&ost->st->metadata, "filename", (p && *p) ? p + 1 : o->attachments[i], AV_DICT_DONT_OVERWRITE);
    }

    return 0;
}

static int create_streams(Muxer *mux, const OptionsContext *o)
{
    static int (* const map_func[])(Muxer *mux, const OptionsContext *o) = {
        [AVMEDIA_TYPE_VIDEO]    = map_auto_video,
        [AVMEDIA_TYPE_AUDIO]    = map_auto_audio,
        [AVMEDIA_TYPE_SUBTITLE] = map_auto_subtitle,
        [AVMEDIA_TYPE_DATA]     = map_auto_data,
    };

    AVFormatContext *oc = mux->fc;

    int auto_disable =
        o->video_disable    * (1 << AVMEDIA_TYPE_VIDEO)    |
        o->audio_disable    * (1 << AVMEDIA_TYPE_AUDIO)    |
        o->subtitle_disable * (1 << AVMEDIA_TYPE_SUBTITLE) |
        o->data_disable     * (1 << AVMEDIA_TYPE_DATA);

    int ret;

    /* create streams for all unlabeled output pads */
    for (int i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        for (int j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *ofilter = fg->outputs[j];

            if (ofilter->linklabel || ofilter->bound)
                continue;

            auto_disable |= 1 << ofilter->type;

            av_log(mux, AV_LOG_VERBOSE, "Creating output stream from unlabeled "
                   "output of complex filtergraph %d.", fg->index);
            if (!o->nb_stream_maps)
                av_log(mux, AV_LOG_VERBOSE, " This overrides automatic %s mapping.",
                       av_get_media_type_string(ofilter->type));
            av_log(mux, AV_LOG_VERBOSE, "\n");

            ret = ost_add(mux, o, ofilter->type, NULL, ofilter, NULL, NULL);
            if (ret < 0)
                return ret;
        }
    }

    if (!o->nb_stream_maps) {
        av_log(mux, AV_LOG_VERBOSE, "No explicit maps, mapping streams automatically...\n");

        /* pick the "best" stream of each type */
        for (int i = 0; i < FF_ARRAY_ELEMS(map_func); i++) {
            if (!map_func[i] || auto_disable & (1 << i))
                continue;
            ret = map_func[i](mux, o);
            if (ret < 0)
                return ret;
        }
    } else {
        av_log(mux, AV_LOG_VERBOSE, "Adding streams from explicit maps...\n");

        for (int i = 0; i < o->nb_stream_maps; i++) {
            ret = map_manual(mux, o, &o->stream_maps[i]);
            if (ret < 0)
                return ret;
        }
    }

    ret = of_add_attachments(mux, o);
    if (ret < 0)
        return ret;

    // setup fix_sub_duration_heartbeat mappings
    for (unsigned i = 0; i < oc->nb_streams; i++) {
        MuxStream *src = ms_from_ost(mux->of.streams[i]);

        if (!src->ost.fix_sub_duration_heartbeat)
            continue;

        for (unsigned j = 0; j < oc->nb_streams; j++) {
            MuxStream *dst = ms_from_ost(mux->of.streams[j]);

            if (src == dst || dst->ost.type != AVMEDIA_TYPE_SUBTITLE ||
                !dst->ost.enc || !dst->ost.ist || !dst->ost.ist->fix_sub_duration)
                continue;

            ret = sch_mux_sub_heartbeat_add(mux->sch, mux->sch_idx, src->sch_idx,
                                            dst->sch_idx_src);

        }
    }

    // handle -apad
    if (o->shortest) {
        int have_video = 0;

        for (unsigned i = 0; i < mux->of.nb_streams; i++)
            if (mux->of.streams[i]->type == AVMEDIA_TYPE_VIDEO) {
                have_video = 1;
                break;
            }

        for (unsigned i = 0; have_video && i < mux->of.nb_streams; i++) {
            MuxStream         *ms = ms_from_ost(mux->of.streams[i]);
            OutputFilter *ofilter = ms->ost.filter;

            if (ms->ost.type != AVMEDIA_TYPE_AUDIO || !ms->apad || !ofilter)
                continue;

            ofilter->apad = av_strdup(ms->apad);
            if (!ofilter->apad)
                return AVERROR(ENOMEM);
        }
    }
    for (unsigned i = 0; i < mux->of.nb_streams; i++) {
        MuxStream *ms = ms_from_ost(mux->of.streams[i]);
        ms->apad = NULL;
    }

    if (!oc->nb_streams && !(oc->oformat->flags & AVFMT_NOSTREAMS)) {
        av_dump_format(oc, nb_output_files - 1, oc->url, 1);
        av_log(mux, AV_LOG_ERROR, "Output file does not contain any stream\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int setup_sync_queues(Muxer *mux, AVFormatContext *oc,
                             int64_t buf_size_us, int shortest)
{
    OutputFile *of = &mux->of;
    int nb_av_enc = 0, nb_audio_fs = 0, nb_interleaved = 0;
    int limit_frames = 0, limit_frames_av_enc = 0;

#define IS_AV_ENC(ost, type)  \
    (ost->enc && (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO))
#define IS_INTERLEAVED(type) (type != AVMEDIA_TYPE_ATTACHMENT)

    for (int i = 0; i < oc->nb_streams; i++) {
        OutputStream *ost = of->streams[i];
        MuxStream     *ms = ms_from_ost(ost);
        enum AVMediaType type = ost->type;

        ms->sq_idx_mux  = -1;

        nb_interleaved += IS_INTERLEAVED(type);
        nb_av_enc      += IS_AV_ENC(ost, type);
        nb_audio_fs    += (ost->enc && type == AVMEDIA_TYPE_AUDIO &&
                           !(ost->enc->enc_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE));

        limit_frames        |=  ms->max_frames < INT64_MAX;
        limit_frames_av_enc |= (ms->max_frames < INT64_MAX) && IS_AV_ENC(ost, type);
    }

    if (!((nb_interleaved > 1 && shortest) ||
          (nb_interleaved > 0 && limit_frames) ||
          nb_audio_fs))
        return 0;

    /* we use a sync queue before encoding when:
     * - 'shortest' is in effect and we have two or more encoded audio/video
     *   streams
     * - at least one encoded audio/video stream is frame-limited, since
     *   that has similar semantics to 'shortest'
     * - at least one audio encoder requires constant frame sizes
     *
     * Note that encoding sync queues are handled in the scheduler, because
     * different encoders run in different threads and need external
     * synchronization, while muxer sync queues can be handled inside the muxer
     */
    if ((shortest && nb_av_enc > 1) || limit_frames_av_enc || nb_audio_fs) {
        int sq_idx, ret;

        sq_idx = sch_add_sq_enc(mux->sch, buf_size_us, mux);
        if (sq_idx < 0)
            return sq_idx;

        for (int i = 0; i < oc->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            MuxStream     *ms = ms_from_ost(ost);
            enum AVMediaType type = ost->type;

            if (!IS_AV_ENC(ost, type))
                continue;

            ret = sch_sq_add_enc(mux->sch, sq_idx, ms->sch_idx_enc,
                                 shortest || ms->max_frames < INT64_MAX,
                                 ms->max_frames);
            if (ret < 0)
                return ret;
        }
    }

    /* if there are any additional interleaved streams, then ALL the streams
     * are also synchronized before sending them to the muxer */
    if (nb_interleaved > nb_av_enc) {
        mux->sq_mux = sq_alloc(SYNC_QUEUE_PACKETS, buf_size_us, mux);
        if (!mux->sq_mux)
            return AVERROR(ENOMEM);

        mux->sq_pkt = av_packet_alloc();
        if (!mux->sq_pkt)
            return AVERROR(ENOMEM);

        for (int i = 0; i < oc->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            MuxStream     *ms = ms_from_ost(ost);
            enum AVMediaType type = ost->type;

            if (!IS_INTERLEAVED(type))
                continue;

            ms->sq_idx_mux = sq_add_stream(mux->sq_mux,
                                           shortest || ms->max_frames < INT64_MAX);
            if (ms->sq_idx_mux < 0)
                return ms->sq_idx_mux;

            if (ms->max_frames != INT64_MAX)
                sq_limit_frames(mux->sq_mux, ms->sq_idx_mux, ms->max_frames);
        }
    }

#undef IS_AV_ENC
#undef IS_INTERLEAVED

    return 0;
}

static int of_parse_iamf_audio_element_layers(Muxer *mux, AVStreamGroup *stg, char *ptr)
{
    AVIAMFAudioElement *audio_element = stg->params.iamf_audio_element;
    AVDictionary *dict = NULL;
    const char *token;
    int ret = 0;

    audio_element->demixing_info =
        av_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_DEMIXING, 1, NULL);
    audio_element->recon_gain_info =
        av_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN, 1, NULL);

    if (!audio_element->demixing_info ||
        !audio_element->recon_gain_info)
        return AVERROR(ENOMEM);

    /* process manually set layers and parameters */
    token = av_strtok(NULL, ",", &ptr);
    while (token) {
        const AVDictionaryEntry *e;
        int demixing = 0, recon_gain = 0;
        int layer = 0;

        if (ptr)
            ptr += strspn(ptr, " \n\t\r");
        if (av_strstart(token, "layer=", &token))
            layer = 1;
        else if (av_strstart(token, "demixing=", &token))
            demixing = 1;
        else if (av_strstart(token, "recon_gain=", &token))
            recon_gain = 1;

        av_dict_free(&dict);
        ret = av_dict_parse_string(&dict, token, "=", ":", 0);
        if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error parsing audio element specification %s\n", token);
            goto fail;
        }

        if (layer) {
            AVIAMFLayer *audio_layer = av_iamf_audio_element_add_layer(audio_element);
            if (!audio_layer) {
                av_log(mux, AV_LOG_ERROR, "Error adding layer to stream group %d\n", stg->index);
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            av_opt_set_dict(audio_layer, &dict);
        } else if (demixing || recon_gain) {
            AVIAMFParamDefinition *param = demixing ? audio_element->demixing_info
                                                    : audio_element->recon_gain_info;
            void *subblock = av_iamf_param_definition_get_subblock(param, 0);

            av_opt_set_dict(param, &dict);
            av_opt_set_dict(subblock, &dict);
        }

        // make sure that no entries are left in the dict
        e = NULL;
        if (e = av_dict_iterate(dict, e)) {
            av_log(mux, AV_LOG_FATAL, "Unknown layer key %s.\n", e->key);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        token = av_strtok(NULL, ",", &ptr);
    }

fail:
    av_dict_free(&dict);
    if (!ret && !audio_element->nb_layers) {
        av_log(mux, AV_LOG_ERROR, "No layer in audio element specification\n");
        ret = AVERROR(EINVAL);
    }

    return ret;
}

static int of_parse_iamf_submixes(Muxer *mux, AVStreamGroup *stg, char *ptr)
{
    AVFormatContext *oc = mux->fc;
    AVIAMFMixPresentation *mix = stg->params.iamf_mix_presentation;
    AVDictionary *dict = NULL;
    const char *token;
    char *submix_str = NULL;
    int ret = 0;

    /* process manually set submixes */
    token = av_strtok(NULL, ",", &ptr);
    while (token) {
        AVIAMFSubmix *submix = NULL;
        const char *subtoken;
        char *subptr = NULL;

        if (ptr)
            ptr += strspn(ptr, " \n\t\r");
        if (!av_strstart(token, "submix=", &token)) {
            av_log(mux, AV_LOG_ERROR, "No submix in mix presentation specification \"%s\"\n", token);
            goto fail;
        }

        submix_str = av_strdup(token);
        if (!submix_str)
            goto fail;

        submix = av_iamf_mix_presentation_add_submix(mix);
        if (!submix) {
            av_log(mux, AV_LOG_ERROR, "Error adding submix to stream group %d\n", stg->index);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        submix->output_mix_config =
            av_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, 0, NULL);
        if (!submix->output_mix_config) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        subptr = NULL;
        subtoken = av_strtok(submix_str, "|", &subptr);
        while (subtoken) {
            const AVDictionaryEntry *e;
            int element = 0, layout = 0;

            if (subptr)
                subptr += strspn(subptr, " \n\t\r");
            if (av_strstart(subtoken, "element=", &subtoken))
                element = 1;
            else if (av_strstart(subtoken, "layout=", &subtoken))
                layout = 1;

            av_dict_free(&dict);
            ret = av_dict_parse_string(&dict, subtoken, "=", ":", 0);
            if (ret < 0) {
                av_log(mux, AV_LOG_ERROR, "Error parsing submix specification \"%s\"\n", subtoken);
                goto fail;
            }

            if (element) {
                AVIAMFSubmixElement *submix_element;
                char *endptr = NULL;
                int64_t idx = -1;

                if (e = av_dict_get(dict, "stg", NULL, 0))
                    idx = strtoll(e->value, &endptr, 0);
                if (!endptr || *endptr || idx < 0 || idx >= oc->nb_stream_groups - 1 ||
                    oc->stream_groups[idx]->type != AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT) {
                    av_log(mux, AV_LOG_ERROR, "Invalid or missing stream group index in "
                                              "submix element specification \"%s\"\n", subtoken);
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                submix_element = av_iamf_submix_add_element(submix);
                if (!submix_element) {
                    av_log(mux, AV_LOG_ERROR, "Error adding element to submix\n");
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }

                submix_element->audio_element_id = oc->stream_groups[idx]->id;

                submix_element->element_mix_config =
                    av_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, 0, NULL);
                if (!submix_element->element_mix_config)
                    ret = AVERROR(ENOMEM);
                av_dict_set(&dict, "stg", NULL, 0);
                av_opt_set_dict2(submix_element, &dict, AV_OPT_SEARCH_CHILDREN);
            } else if (layout) {
                AVIAMFSubmixLayout *submix_layout = av_iamf_submix_add_layout(submix);
                if (!submix_layout) {
                    av_log(mux, AV_LOG_ERROR, "Error adding layout to submix\n");
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                av_opt_set_dict(submix_layout, &dict);
            } else
                av_opt_set_dict2(submix, &dict, AV_OPT_SEARCH_CHILDREN);

            if (ret < 0) {
                goto fail;
            }

            // make sure that no entries are left in the dict
            e = NULL;
            while (e = av_dict_iterate(dict, e)) {
                av_log(mux, AV_LOG_FATAL, "Unknown submix key %s.\n", e->key);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            subtoken = av_strtok(NULL, "|", &subptr);
        }
        av_freep(&submix_str);

        if (!submix->nb_elements) {
            av_log(mux, AV_LOG_ERROR, "No audio elements in submix specification \"%s\"\n", token);
            ret = AVERROR(EINVAL);
        }
        token = av_strtok(NULL, ",", &ptr);
    }

fail:
    av_dict_free(&dict);
    av_free(submix_str);

    return ret;
}

static int of_serialize_options(Muxer *mux, void *obj, AVBPrint *bp)
{
    char *ptr;
    int ret;

    ret = av_opt_serialize(obj, 0, AV_OPT_SERIALIZE_SKIP_DEFAULTS | AV_OPT_SERIALIZE_SEARCH_CHILDREN,
                           &ptr, '=', ':');
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Failed to serialize group\n");
        return ret;
    }

    av_bprintf(bp, "%s", ptr);
    ret = strlen(ptr);
    av_free(ptr);

    return ret;
}

#define SERIALIZE(parent, child) do {                   \
    ret = of_serialize_options(mux, parent->child, bp); \
    if (ret < 0)                                        \
        return ret;                                     \
} while (0)

#define SERIALIZE_LOOP_SUBBLOCK(obj) do {                                \
    for (int k = 0; k < obj->nb_subblocks; k++) {                        \
        ret = of_serialize_options(mux,                                  \
                  av_iamf_param_definition_get_subblock(obj, k), bp);    \
        if (ret < 0)                                                     \
            return ret;                                                  \
    }                                                                    \
} while (0)

#define SERIALIZE_LOOP(parent, child, suffix, separator) do {            \
    for (int j = 0; j < parent->nb_## child ## suffix; j++) {            \
        av_bprintf(bp, separator#child "=");                             \
        SERIALIZE(parent, child ## suffix[j]);                           \
    }                                                                    \
} while (0)

static int64_t get_stream_group_index_from_id(Muxer *mux, int64_t id)
{
    AVFormatContext *oc = mux->fc;

    for (unsigned i = 0; i < oc->nb_stream_groups; i++)
        if (oc->stream_groups[i]->id == id)
            return oc->stream_groups[i]->index;

    return AVERROR(EINVAL);
}

static int of_map_group(Muxer *mux, AVDictionary **dict, AVBPrint *bp, const char *map)
{
    AVStreamGroup *stg;
    int ret, file_idx, stream_idx;
    char *ptr;

    file_idx = strtol(map, &ptr, 0);
    if (file_idx >= nb_input_files || file_idx < 0 || map == ptr) {
        av_log(mux, AV_LOG_ERROR, "Invalid input file index: %d.\n", file_idx);
        return AVERROR(EINVAL);
    }

    stream_idx = strtol(*ptr == '=' ? ptr + 1 : ptr, &ptr, 0);
    if (*ptr || stream_idx >= input_files[file_idx]->ctx->nb_stream_groups || stream_idx < 0) {
        av_log(mux, AV_LOG_ERROR, "Invalid input stream group index: %d.\n", stream_idx);
        return AVERROR(EINVAL);
    }

    stg = input_files[file_idx]->ctx->stream_groups[stream_idx];
    ret = of_serialize_options(mux, stg, bp);
    if (ret < 0)
       return ret;

    ret = av_dict_parse_string(dict, bp->str, "=", ":", 0);
    if (ret < 0)
        av_log(mux, AV_LOG_ERROR, "Error parsing mapped group specification %s\n", ptr);
    av_dict_set_int(dict, "type", stg->type, 0);

    av_bprint_clear(bp);
    switch(stg->type) {
    case AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT: {
        AVIAMFAudioElement *audio_element = stg->params.iamf_audio_element;

        if (audio_element->demixing_info) {
            AVIAMFParamDefinition *demixing_info = audio_element->demixing_info;
            av_bprintf(bp, ",demixing=");
            SERIALIZE(audio_element, demixing_info);
            if (ret && demixing_info->nb_subblocks)
                av_bprintf(bp, ":");
            SERIALIZE_LOOP_SUBBLOCK(demixing_info);
        }
        if (audio_element->recon_gain_info) {
            AVIAMFParamDefinition *recon_gain_info = audio_element->recon_gain_info;
            av_bprintf(bp, ",recon_gain=");
            SERIALIZE(audio_element, recon_gain_info);
            if (ret && recon_gain_info->nb_subblocks)
                av_bprintf(bp, ":");
            SERIALIZE_LOOP_SUBBLOCK(recon_gain_info);
        }
        SERIALIZE_LOOP(audio_element, layer, s, ",");
        break;
    }
    case AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION: {
        AVIAMFMixPresentation *mix = stg->params.iamf_mix_presentation;

        for (int i = 0; i < mix->nb_submixes; i++) {
            AVIAMFSubmix *submix = mix->submixes[i];
            AVIAMFParamDefinition *output_mix_config = submix->output_mix_config;

            av_bprintf(bp, ",submix=");
            SERIALIZE(mix, submixes[i]);
            if (ret && output_mix_config->nb_subblocks)
                av_bprintf(bp, ":");
            SERIALIZE_LOOP_SUBBLOCK(output_mix_config);
            for (int j = 0; j < submix->nb_elements; j++) {
                AVIAMFSubmixElement *element = submix->elements[j];
                AVIAMFParamDefinition *element_mix_config = element->element_mix_config;
                int64_t id = get_stream_group_index_from_id(mux, element->audio_element_id);

                if (id < 0) {
                    av_log(mux, AV_LOG_ERROR, "Invalid or missing stream group index in"
                                              "submix element");
                    return id;
                }

                av_bprintf(bp, "|element=");
                SERIALIZE(submix, elements[j]);
                if (ret && element_mix_config->nb_subblocks)
                    av_bprintf(bp, ":");
                SERIALIZE_LOOP_SUBBLOCK(element_mix_config);
                if (ret)
                    av_bprintf(bp, ":");
                av_bprintf(bp, "stg=%"PRId64, id);
            }
            SERIALIZE_LOOP(submix, layout, s, "|");
        }
        break;
    }
    default:
        av_log(mux, AV_LOG_ERROR, "Unsupported mapped group type %d.\n", stg->type);
        ret = AVERROR(EINVAL);
        break;
    }
    return 0;
}

static int of_parse_group_token(Muxer *mux, const char *token, char *ptr)
{
    AVFormatContext *oc = mux->fc;
    AVStreamGroup *stg;
    AVDictionary *dict = NULL, *tmp = NULL;
    char *mapped_string = NULL;
    const AVDictionaryEntry *e;
    const AVOption opts[] = {
        { "type", "Set group type", offsetof(AVStreamGroup, type), AV_OPT_TYPE_INT,
                { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM, .unit = "type" },
            { "iamf_audio_element",    NULL, 0, AV_OPT_TYPE_CONST,
                { .i64 = AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT },    .unit = "type" },
            { "iamf_mix_presentation", NULL, 0, AV_OPT_TYPE_CONST,
                { .i64 = AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION }, .unit = "type" },
        { NULL },
    };
    const AVClass class = {
        .class_name = "StreamGroupType",
        .item_name  = av_default_item_name,
        .option     = opts,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *pclass = &class;
    int type, ret;

    ret = av_dict_parse_string(&dict, token, "=", ":", AV_DICT_MULTIKEY);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Error parsing group specification %s\n", token);
        return ret;
    }

    av_dict_copy(&tmp, dict, 0);
    e = av_dict_get(dict, "map", NULL, 0);
    if (e) {
        AVBPrint bp;

        if (ptr) {
            av_log(mux, AV_LOG_ERROR, "Unexpected extra parameters when mapping a"
                                      " stream group\n");
            ret = AVERROR(EINVAL);
            goto end;
        }

        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
        ret = of_map_group(mux, &tmp, &bp, e->value);
        if (ret < 0) {
            av_bprint_finalize(&bp, NULL);
            goto end;
        }

        av_bprint_finalize(&bp, &mapped_string);
        ptr = mapped_string;
    }

    // "type" is not a user settable AVOption in AVStreamGroup, so handle it here
    e = av_dict_get(tmp, "type", NULL, 0);
    if (!e) {
        av_log(mux, AV_LOG_ERROR, "No type specified for Stream Group in \"%s\"\n", token);
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = av_opt_eval_int(&pclass, opts, e->value, &type);
    if (!ret && type == AV_STREAM_GROUP_PARAMS_NONE)
        ret = AVERROR(EINVAL);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Invalid group type \"%s\"\n", e->value);
        goto end;
    }

    stg = avformat_stream_group_create(oc, type, &tmp);
    if (!stg) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    e = NULL;
    while (e = av_dict_get(dict, "st", e, 0)) {
        char *endptr;
        int64_t idx = strtoll(e->value, &endptr, 0);
        if (*endptr || idx < 0 || idx >= oc->nb_streams) {
            av_log(mux, AV_LOG_ERROR, "Invalid stream index %"PRId64"\n", idx);
            ret = AVERROR(EINVAL);
            goto end;
        }
        ret = avformat_stream_group_add_stream(stg, oc->streams[idx]);
        if (ret < 0)
            goto end;
    }
    while (e = av_dict_get(dict, "stg", e, 0)) {
        char *endptr;
        int64_t idx = strtoll(e->value, &endptr, 0);
        if (*endptr || idx < 0 || idx >= oc->nb_stream_groups - 1) {
            av_log(mux, AV_LOG_ERROR, "Invalid stream group index %"PRId64"\n", idx);
            ret = AVERROR(EINVAL);
            goto end;
        }
        for (unsigned i = 0; i < oc->stream_groups[idx]->nb_streams; i++) {
            ret = avformat_stream_group_add_stream(stg, oc->stream_groups[idx]->streams[i]);
            if (ret < 0)
                goto end;
        }
    }

    switch(type) {
    case AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT:
        ret = of_parse_iamf_audio_element_layers(mux, stg, ptr);
        break;
    case AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION:
        ret = of_parse_iamf_submixes(mux, stg, ptr);
        break;
    default:
        av_log(mux, AV_LOG_FATAL, "Unknown group type %d.\n", type);
        ret = AVERROR(EINVAL);
        break;
    }

    if (ret < 0)
        goto end;

    // make sure that nothing but "st" and "stg" entries are left in the dict
    e = NULL;
    av_dict_set(&tmp, "map", NULL, 0);
    av_dict_set(&tmp, "type", NULL, 0);
    while (e = av_dict_iterate(tmp, e)) {
        if (!strcmp(e->key, "st") || !strcmp(e->key, "stg"))
            continue;

        av_log(mux, AV_LOG_FATAL, "Unknown group key %s.\n", e->key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = 0;
end:
    av_free(mapped_string);
    av_dict_free(&dict);
    av_dict_free(&tmp);

    return ret;
}

static int of_add_groups(Muxer *mux, const OptionsContext *o)
{
    /* process manually set groups */
    for (int i = 0; i < o->stream_groups.nb_opt; i++) {
        const char *token;
        char *str, *ptr = NULL;
        int ret = 0;

        str = av_strdup(o->stream_groups.opt[i].u.str);
        if (!str)
            return ret;

        token = av_strtok(str, ",", &ptr);
        if (token) {
            if (ptr)
                ptr += strspn(ptr, " \n\t\r");
            ret = of_parse_group_token(mux, token, ptr);
        }

        av_free(str);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int of_add_programs(Muxer *mux, const OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    /* process manually set programs */
    for (int i = 0; i < o->program.nb_opt; i++) {
        AVDictionary *dict = NULL;
        const AVDictionaryEntry *e;
        AVProgram *program;
        int ret, progid = i + 1;

        ret = av_dict_parse_string(&dict, o->program.opt[i].u.str, "=", ":",
                                   AV_DICT_MULTIKEY);
        if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error parsing program specification %s\n",
                   o->program.opt[i].u.str);
            return ret;
        }

        e = av_dict_get(dict, "program_num", NULL, 0);
        if (e) {
            progid = strtol(e->value, NULL, 0);
            av_dict_set(&dict, e->key, NULL, 0);
        }

        program = av_new_program(oc, progid);
        if (!program) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        e = av_dict_get(dict, "title", NULL, 0);
        if (e) {
            av_dict_set(&program->metadata, e->key, e->value, 0);
            av_dict_set(&dict, e->key, NULL, 0);
        }

        e = NULL;
        while (e = av_dict_get(dict, "st", e, 0)) {
            int st_num = strtol(e->value, NULL, 0);
            av_program_add_stream_index(oc, progid, st_num);
        }

        // make sure that nothing but "st" entries are left in the dict
        e = NULL;
        while (e = av_dict_iterate(dict, e)) {
            if (!strcmp(e->key, "st"))
                continue;

            av_log(mux, AV_LOG_FATAL, "Unknown program key %s.\n", e->key);
            ret = AVERROR(EINVAL);
            goto fail;
        }

fail:
        av_dict_free(&dict);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Parse a metadata specifier passed as 'arg' parameter.
 * @param arg  metadata string to parse
 * @param type metadata type is written here -- g(lobal)/s(tream)/c(hapter)/p(rogram)
 * @param index for type c/p, chapter/program index is written here
 * @param stream_spec for type s, the stream specifier is written here
 */
static int parse_meta_type(void *logctx, const char *arg,
                           char *type, int *index, const char **stream_spec)
{
    if (*arg) {
        *type = *arg;
        switch (*arg) {
        case 'g':
            break;
        case 's':
            if (*(++arg) && *arg != ':') {
                av_log(logctx, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", arg);
                return AVERROR(EINVAL);
            }
            *stream_spec = *arg == ':' ? arg + 1 : "";
            break;
        case 'c':
        case 'p':
            if (*(++arg) == ':')
                *index = strtol(++arg, NULL, 0);
            break;
        default:
            av_log(logctx, AV_LOG_FATAL, "Invalid metadata type %c.\n", *arg);
            return AVERROR(EINVAL);
        }
    } else
        *type = 'g';

    return 0;
}

static int of_add_metadata(OutputFile *of, AVFormatContext *oc,
                           const OptionsContext *o)
{
    for (int i = 0; i < o->metadata.nb_opt; i++) {
        AVDictionary **m;
        char type, *val;
        const char *stream_spec;
        int index = 0, ret = 0;

        val = strchr(o->metadata.opt[i].u.str, '=');
        if (!val) {
            av_log(of, AV_LOG_FATAL, "No '=' character in metadata string %s.\n",
                   o->metadata.opt[i].u.str);
            return AVERROR(EINVAL);
        }
        *val++ = 0;

        ret = parse_meta_type(of, o->metadata.opt[i].specifier, &type, &index, &stream_spec);
        if (ret < 0)
            return ret;

        if (type == 's') {
            for (int j = 0; j < oc->nb_streams; j++) {
                if ((ret = check_stream_specifier(oc, oc->streams[j], stream_spec)) > 0) {
                    av_dict_set(&oc->streams[j]->metadata, o->metadata.opt[i].u.str, *val ? val : NULL, 0);
                } else if (ret < 0)
                    return ret;
            }
        } else {
            switch (type) {
            case 'g':
                m = &oc->metadata;
                break;
            case 'c':
                if (index < 0 || index >= oc->nb_chapters) {
                    av_log(of, AV_LOG_FATAL, "Invalid chapter index %d in metadata specifier.\n", index);
                    return AVERROR(EINVAL);
                }
                m = &oc->chapters[index]->metadata;
                break;
            case 'p':
                if (index < 0 || index >= oc->nb_programs) {
                    av_log(of, AV_LOG_FATAL, "Invalid program index %d in metadata specifier.\n", index);
                    return AVERROR(EINVAL);
                }
                m = &oc->programs[index]->metadata;
                break;
            default:
                av_log(of, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", o->metadata.opt[i].specifier);
                return AVERROR(EINVAL);
            }
            av_dict_set(m, o->metadata.opt[i].u.str, *val ? val : NULL, 0);
        }
    }

    return 0;
}

static int copy_chapters(InputFile *ifile, OutputFile *ofile, AVFormatContext *os,
                         int copy_metadata)
{
    AVFormatContext *is = ifile->ctx;
    AVChapter **tmp;

    tmp = av_realloc_f(os->chapters, is->nb_chapters + os->nb_chapters, sizeof(*os->chapters));
    if (!tmp)
        return AVERROR(ENOMEM);
    os->chapters = tmp;

    for (int i = 0; i < is->nb_chapters; i++) {
        AVChapter *in_ch = is->chapters[i], *out_ch;
        int64_t start_time = (ofile->start_time == AV_NOPTS_VALUE) ? 0 : ofile->start_time;
        int64_t ts_off   = av_rescale_q(start_time - ifile->ts_offset,
                                       AV_TIME_BASE_Q, in_ch->time_base);
        int64_t rt       = (ofile->recording_time == INT64_MAX) ? INT64_MAX :
                           av_rescale_q(ofile->recording_time, AV_TIME_BASE_Q, in_ch->time_base);


        if (in_ch->end < ts_off)
            continue;
        if (rt != INT64_MAX && in_ch->start > rt + ts_off)
            break;

        out_ch = av_mallocz(sizeof(AVChapter));
        if (!out_ch)
            return AVERROR(ENOMEM);

        out_ch->id        = in_ch->id;
        out_ch->time_base = in_ch->time_base;
        out_ch->start     = FFMAX(0,  in_ch->start - ts_off);
        out_ch->end       = FFMIN(rt, in_ch->end   - ts_off);

        if (copy_metadata)
            av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);

        os->chapters[os->nb_chapters++] = out_ch;
    }
    return 0;
}

static int copy_metadata(Muxer *mux, AVFormatContext *ic,
                         const char *outspec, const char *inspec,
                         int *metadata_global_manual, int *metadata_streams_manual,
                         int *metadata_chapters_manual)
{
    AVFormatContext *oc = mux->fc;
    AVDictionary **meta_in = NULL;
    AVDictionary **meta_out = NULL;
    int i, ret = 0;
    char type_in, type_out;
    const char *istream_spec = NULL, *ostream_spec = NULL;
    int idx_in = 0, idx_out = 0;

    ret     = parse_meta_type(mux, inspec,  &type_in,  &idx_in,  &istream_spec);
    if (ret >= 0)
        ret = parse_meta_type(mux, outspec, &type_out, &idx_out, &ostream_spec);
    if (ret < 0)
        return ret;

    if (type_in == 'g' || type_out == 'g' || (!*outspec && !ic))
        *metadata_global_manual = 1;
    if (type_in == 's' || type_out == 's' || (!*outspec && !ic))
        *metadata_streams_manual = 1;
    if (type_in == 'c' || type_out == 'c' || (!*outspec && !ic))
        *metadata_chapters_manual = 1;

    /* ic is NULL when just disabling automatic mappings */
    if (!ic)
        return 0;

#define METADATA_CHECK_INDEX(index, nb_elems, desc)\
    if ((index) < 0 || (index) >= (nb_elems)) {\
        av_log(mux, AV_LOG_FATAL, "Invalid %s index %d while processing metadata maps.\n",\
                (desc), (index));\
        return AVERROR(EINVAL);\
    }

#define SET_DICT(type, meta, context, index)\
        switch (type) {\
        case 'g':\
            meta = &context->metadata;\
            break;\
        case 'c':\
            METADATA_CHECK_INDEX(index, context->nb_chapters, "chapter")\
            meta = &context->chapters[index]->metadata;\
            break;\
        case 'p':\
            METADATA_CHECK_INDEX(index, context->nb_programs, "program")\
            meta = &context->programs[index]->metadata;\
            break;\
        case 's':\
            break; /* handled separately below */ \
        default: av_assert0(0);\
        }\

    SET_DICT(type_in, meta_in, ic, idx_in);
    SET_DICT(type_out, meta_out, oc, idx_out);

    /* for input streams choose first matching stream */
    if (type_in == 's') {
        for (i = 0; i < ic->nb_streams; i++) {
            if ((ret = check_stream_specifier(ic, ic->streams[i], istream_spec)) > 0) {
                meta_in = &ic->streams[i]->metadata;
                break;
            } else if (ret < 0)
                return ret;
        }
        if (!meta_in) {
            av_log(mux, AV_LOG_FATAL, "Stream specifier %s does not match  any streams.\n", istream_spec);
            return AVERROR(EINVAL);
        }
    }

    if (type_out == 's') {
        for (i = 0; i < oc->nb_streams; i++) {
            if ((ret = check_stream_specifier(oc, oc->streams[i], ostream_spec)) > 0) {
                meta_out = &oc->streams[i]->metadata;
                av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);
            } else if (ret < 0)
                return ret;
        }
    } else
        av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);

    return 0;
}

static int copy_meta(Muxer *mux, const OptionsContext *o)
{
    OutputFile      *of = &mux->of;
    AVFormatContext *oc = mux->fc;
    int chapters_input_file = o->chapters_input_file;
    int metadata_global_manual   = 0;
    int metadata_streams_manual  = 0;
    int metadata_chapters_manual = 0;
    int ret;

    /* copy metadata */
    for (int i = 0; i < o->metadata_map.nb_opt; i++) {
        char *p;
        int in_file_index = strtol(o->metadata_map.opt[i].u.str, &p, 0);

        if (in_file_index >= nb_input_files) {
            av_log(mux, AV_LOG_FATAL, "Invalid input file index %d while "
                   "processing metadata maps\n", in_file_index);
            return AVERROR(EINVAL);
        }
        ret = copy_metadata(mux,
                            in_file_index >= 0 ? input_files[in_file_index]->ctx : NULL,
                            o->metadata_map.opt[i].specifier, *p ? p + 1 : p,
                            &metadata_global_manual, &metadata_streams_manual,
                            &metadata_chapters_manual);
        if (ret < 0)
            return ret;
    }

    /* copy chapters */
    if (chapters_input_file >= nb_input_files) {
        if (chapters_input_file == INT_MAX) {
            /* copy chapters from the first input file that has them*/
            chapters_input_file = -1;
            for (int i = 0; i < nb_input_files; i++)
                if (input_files[i]->ctx->nb_chapters) {
                    chapters_input_file = i;
                    break;
                }
        } else {
            av_log(mux, AV_LOG_FATAL, "Invalid input file index %d in chapter mapping.\n",
                   chapters_input_file);
            return AVERROR(EINVAL);
        }
    }
    if (chapters_input_file >= 0)
        copy_chapters(input_files[chapters_input_file], of, oc,
                      !metadata_chapters_manual);

    /* copy global metadata by default */
    if (!metadata_global_manual && nb_input_files){
        av_dict_copy(&oc->metadata, input_files[0]->ctx->metadata,
                     AV_DICT_DONT_OVERWRITE);
        if (of->recording_time != INT64_MAX)
            av_dict_set(&oc->metadata, "duration", NULL, 0);
        av_dict_set(&oc->metadata, "creation_time", NULL, 0);
        av_dict_set(&oc->metadata, "company_name", NULL, 0);
        av_dict_set(&oc->metadata, "product_name", NULL, 0);
        av_dict_set(&oc->metadata, "product_version", NULL, 0);
    }
    if (!metadata_streams_manual)
        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];

            if (!ost->ist)         /* this is true e.g. for attached files */
                continue;
            av_dict_copy(&ost->st->metadata, ost->ist->st->metadata, AV_DICT_DONT_OVERWRITE);
        }

    return 0;
}

static int set_dispositions(Muxer *mux, const OptionsContext *o)
{
    OutputFile                    *of = &mux->of;
    AVFormatContext              *ctx = mux->fc;

    // indexed by type+1, because AVMEDIA_TYPE_UNKNOWN=-1
    int nb_streams[AVMEDIA_TYPE_NB + 1]   = { 0 };
    int have_default[AVMEDIA_TYPE_NB + 1] = { 0 };
    int have_manual = 0;
    int ret = 0;

    const char **dispositions;

    dispositions = av_calloc(ctx->nb_streams, sizeof(*dispositions));
    if (!dispositions)
        return AVERROR(ENOMEM);

    // first, copy the input dispositions
    for (int i = 0; i < ctx->nb_streams; i++) {
        OutputStream *ost = of->streams[i];

        nb_streams[ost->type + 1]++;

        opt_match_per_stream_str(ost, &o->disposition, ctx, ost->st, &dispositions[i]);

        have_manual |= !!dispositions[i];

        if (ost->ist) {
            ost->st->disposition = ost->ist->st->disposition;

            if (ost->st->disposition & AV_DISPOSITION_DEFAULT)
                have_default[ost->type + 1] = 1;
        }
    }

    if (have_manual) {
        // process manually set dispositions - they override the above copy
        for (int i = 0; i < ctx->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            const char  *disp = dispositions[i];

            if (!disp)
                continue;

            ret = av_opt_set(ost->st, "disposition", disp, 0);
            if (ret < 0)
                goto finish;
        }
    } else {
        // For each media type with more than one stream, find a suitable stream to
        // mark as default, unless one is already marked default.
        // "Suitable" means the first of that type, skipping attached pictures.
        for (int i = 0; i < ctx->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            enum AVMediaType type = ost->type;

            if (nb_streams[type + 1] < 2 || have_default[type + 1] ||
                ost->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;

            ost->st->disposition |= AV_DISPOSITION_DEFAULT;
            have_default[type + 1] = 1;
        }
    }

finish:
    av_freep(&dispositions);

    return ret;
}

static const char *const forced_keyframes_const_names[] = {
    "n",
    "n_forced",
    "prev_forced_n",
    "prev_forced_t",
    "t",
    NULL
};

static int compare_int64(const void *a, const void *b)
{
    return FFDIFFSIGN(*(const int64_t *)a, *(const int64_t *)b);
}

static int parse_forced_key_frames(void *log, KeyframeForceCtx *kf,
                                   const Muxer *mux, const char *spec)
{
    const char *p;
    int n = 1, i, ret, size, index = 0;
    int64_t t, *pts;

    for (p = spec; *p; p++)
        if (*p == ',')
            n++;
    size = n;
    pts = av_malloc_array(size, sizeof(*pts));
    if (!pts)
        return AVERROR(ENOMEM);

    p = spec;
    for (i = 0; i < n; i++) {
        char *next = strchr(p, ',');

        if (next)
            *next++ = 0;

        if (strstr(p, "chapters") == p) {
            AVChapter * const *ch = mux->fc->chapters;
            unsigned int    nb_ch = mux->fc->nb_chapters;
            int j;

            if (nb_ch > INT_MAX - size) {
                ret = AVERROR(ERANGE);
                goto fail;
            }
            size += nb_ch - 1;
            pts = av_realloc_f(pts, size, sizeof(*pts));
            if (!pts)
                return AVERROR(ENOMEM);

            if (p[8]) {
                ret = av_parse_time(&t, p + 8, 1);
                if (ret < 0) {
                    av_log(log, AV_LOG_ERROR,
                           "Invalid chapter time offset: %s\n", p + 8);
                    goto fail;
                }
            } else
                t = 0;

            for (j = 0; j < nb_ch; j++) {
                const AVChapter *c = ch[j];
                av_assert1(index < size);
                pts[index++] = av_rescale_q(c->start, c->time_base,
                                            AV_TIME_BASE_Q) + t;
            }

        } else {
            av_assert1(index < size);
            ret = av_parse_time(&t, p, 1);
            if (ret < 0) {
                av_log(log, AV_LOG_ERROR, "Invalid keyframe time: %s\n", p);
                goto fail;
            }

            pts[index++] = t;
        }

        p = next;
    }

    av_assert0(index == size);
    qsort(pts, size, sizeof(*pts), compare_int64);
    kf->nb_pts = size;
    kf->pts    = pts;

    return 0;
fail:
    av_freep(&pts);
    return ret;
}

static int process_forced_keyframes(Muxer *mux, const OptionsContext *o)
{
    for (int i = 0; i < mux->of.nb_streams; i++) {
        OutputStream *ost = mux->of.streams[i];
        const char *forced_keyframes = NULL;

        opt_match_per_stream_str(ost, &o->forced_key_frames,
                                 mux->fc, ost->st, &forced_keyframes);

        if (!(ost->type == AVMEDIA_TYPE_VIDEO &&
              ost->enc && forced_keyframes))
            continue;

        if (!strncmp(forced_keyframes, "expr:", 5)) {
            int ret = av_expr_parse(&ost->kf.pexpr, forced_keyframes + 5,
                                    forced_keyframes_const_names, NULL, NULL, NULL, NULL, 0, NULL);
            if (ret < 0) {
                av_log(ost, AV_LOG_ERROR,
                       "Invalid force_key_frames expression '%s'\n", forced_keyframes + 5);
                return ret;
            }
            ost->kf.expr_const_values[FKF_N]             = 0;
            ost->kf.expr_const_values[FKF_N_FORCED]      = 0;
            ost->kf.expr_const_values[FKF_PREV_FORCED_N] = NAN;
            ost->kf.expr_const_values[FKF_PREV_FORCED_T] = NAN;

            // Don't parse the 'forced_keyframes' in case of 'keep-source-keyframes',
            // parse it only for static kf timings
        } else if (!strcmp(forced_keyframes, "source")) {
            ost->kf.type = KF_FORCE_SOURCE;
#if FFMPEG_OPT_FORCE_KF_SOURCE_NO_DROP
        } else if (!strcmp(forced_keyframes, "source_no_drop")) {
            av_log(ost, AV_LOG_WARNING, "The 'source_no_drop' value for "
                   "-force_key_frames is deprecated, use just 'source'\n");
            ost->kf.type = KF_FORCE_SOURCE;
#endif
        } else if (!strcmp(forced_keyframes, "scd_metadata")) {
            ost->kf.type = KF_FORCE_SCD_METADATA;
        } else {
            int ret = parse_forced_key_frames(ost, &ost->kf, mux, forced_keyframes);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static const char *output_file_item_name(void *obj)
{
    const Muxer *mux = obj;

    return mux->log_name;
}

static const AVClass output_file_class = {
    .class_name = "OutputFile",
    .version    = LIBAVUTIL_VERSION_INT,
    .item_name  = output_file_item_name,
    .category   = AV_CLASS_CATEGORY_MUXER,
};

static Muxer *mux_alloc(void)
{
    Muxer *mux = allocate_array_elem(&output_files, sizeof(*mux), &nb_output_files);

    if (!mux)
        return NULL;

    mux->of.class = &output_file_class;
    mux->of.index = nb_output_files - 1;

    snprintf(mux->log_name, sizeof(mux->log_name), "out#%d", mux->of.index);

    return mux;
}

int of_open(const OptionsContext *o, const char *filename, Scheduler *sch)
{
    Muxer *mux;
    AVFormatContext *oc;
    int err;
    OutputFile *of;

    int64_t recording_time = o->recording_time;
    int64_t stop_time      = o->stop_time;

    mux = mux_alloc();
    if (!mux)
        return AVERROR(ENOMEM);

    of  = &mux->of;

    if (stop_time != INT64_MAX && recording_time != INT64_MAX) {
        stop_time = INT64_MAX;
        av_log(mux, AV_LOG_WARNING, "-t and -to cannot be used together; using -t.\n");
    }

    if (stop_time != INT64_MAX && recording_time == INT64_MAX) {
        int64_t start_time = o->start_time == AV_NOPTS_VALUE ? 0 : o->start_time;
        if (stop_time <= start_time) {
            av_log(mux, AV_LOG_ERROR, "-to value smaller than -ss; aborting.\n");
            return AVERROR(EINVAL);
        } else {
            recording_time = stop_time - start_time;
        }
    }

    of->recording_time = recording_time;
    of->start_time     = o->start_time;

    mux->limit_filesize    = o->limit_filesize;
    av_dict_copy(&mux->opts, o->g->format_opts, 0);

    if (!strcmp(filename, "-"))
        filename = "pipe:";

    err = avformat_alloc_output_context2(&oc, NULL, o->format, filename);
    if (!oc) {
        av_log(mux, AV_LOG_FATAL, "Error initializing the muxer for %s: %s\n",
               filename, av_err2str(err));
        return err;
    }
    mux->fc = oc;

    av_strlcat(mux->log_name, "/",               sizeof(mux->log_name));
    av_strlcat(mux->log_name, oc->oformat->name, sizeof(mux->log_name));


    if (recording_time != INT64_MAX)
        oc->duration = recording_time;

    oc->interrupt_callback = int_cb;

    if (o->bitexact) {
        oc->flags    |= AVFMT_FLAG_BITEXACT;
        of->bitexact  = 1;
    } else {
        of->bitexact  = check_opt_bitexact(oc, mux->opts, "fflags",
                                           AVFMT_FLAG_BITEXACT);
    }

    err = sch_add_mux(sch, muxer_thread, mux_check_init, mux,
                      !strcmp(oc->oformat->name, "rtp"), o->thread_queue_size);
    if (err < 0)
        return err;
    mux->sch     = sch;
    mux->sch_idx = err;

    /* create all output streams for this file */
    err = create_streams(mux, o);
    if (err < 0)
        return err;

    /* check if all codec options have been used */
    err = check_avoptions_used(o->g->codec_opts, mux->enc_opts_used, mux, 0);
    av_dict_free(&mux->enc_opts_used);
    if (err < 0)
        return err;

    /* check filename in case of an image number is expected */
    if (oc->oformat->flags & AVFMT_NEEDNUMBER && !av_filename_number_test(oc->url)) {
        av_log(mux, AV_LOG_FATAL,
               "Output filename '%s' does not contain a numeric pattern like "
               "'%%d', which is required by output format '%s'.\n",
               oc->url, oc->oformat->name);
        return AVERROR(EINVAL);
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        /* test if it already exists to avoid losing precious files */
        err = assert_file_overwrite(filename);
        if (err < 0)
            return err;

        /* open the file */
        if ((err = avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE,
                              &oc->interrupt_callback,
                              &mux->opts)) < 0) {
            av_log(mux, AV_LOG_FATAL, "Error opening output %s: %s\n",
                   filename, av_err2str(err));
            return err;
        }
    } else if (strcmp(oc->oformat->name, "image2")==0 && !av_filename_number_test(filename)) {
        err = assert_file_overwrite(filename);
        if (err < 0)
            return err;
    }

    if (o->mux_preload) {
        av_dict_set_int(&mux->opts, "preload", o->mux_preload*AV_TIME_BASE, 0);
    }
    oc->max_delay = (int)(o->mux_max_delay * AV_TIME_BASE);

    /* copy metadata and chapters from input files */
    err = copy_meta(mux, o);
    if (err < 0)
        return err;

    err = of_add_groups(mux, o);
    if (err < 0)
        return err;

    err = of_add_programs(mux, o);
    if (err < 0)
        return err;

    err = of_add_metadata(of, oc, o);
    if (err < 0)
        return err;

    err = set_dispositions(mux, o);
    if (err < 0) {
        av_log(mux, AV_LOG_FATAL, "Error setting output stream dispositions\n");
        return err;
    }

    // parse forced keyframe specifications;
    // must be done after chapters are created
    err = process_forced_keyframes(mux, o);
    if (err < 0) {
        av_log(mux, AV_LOG_FATAL, "Error processing forced keyframes\n");
        return err;
    }

    err = setup_sync_queues(mux, oc, o->shortest_buf_duration * AV_TIME_BASE,
                            o->shortest);
    if (err < 0) {
        av_log(mux, AV_LOG_FATAL, "Error setting up output sync queues\n");
        return err;
    }

    of->url        = filename;

    /* initialize streamcopy streams. */
    for (int i = 0; i < of->nb_streams; i++) {
        OutputStream *ost = of->streams[i];

        if (!ost->enc) {
            err = of_stream_init(of, ost, NULL);
            if (err < 0)
                return err;
        }
    }

    return 0;
}


/* ========== fftools/ffmpeg_opt.c ========== */

/*
 * ffmpeg option parsing
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

#include "config.h"

#include <stdint.h>

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "ffmpeg.h"
#include "ffmpeg_sched.h"
#include "cmdutils.h"
#include "opt_common.h"

#include "libavformat/avformat.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/stereo3d.h"
#include "graphprint.h"

HWDevice *filter_hw_device;

char *vstats_filename;

float dts_delta_threshold   = 10;
float dts_error_threshold   = 3600*30;

#if FFMPEG_OPT_VSYNC
enum VideoSyncMethod video_sync_method = VSYNC_AUTO;
#endif
float frame_drop_threshold = 0;
int do_benchmark      = 0;
int do_benchmark_all  = 0;
int do_hex_dump       = 0;
int do_pkt_dump       = 0;
int copy_ts           = 0;
int start_at_zero     = 0;
int copy_tb           = -1;
int debug_ts          = 0;
int exit_on_error     = 0;
int abort_on_flags    = 0;
int print_stats       = -1;
int stdin_interaction = 1;
float max_error_rate  = 2.0/3;
char *filter_nbthreads;
int filter_complex_nbthreads = 0;
int filter_buffered_frames = 0;
int vstats_version = 2;
/* print_graphs defs moved to common.c */
int auto_conversion_filters = 1;
int64_t stats_period = 500000;


static int file_overwrite     = 0;
static int no_file_overwrite  = 0;
int ignore_unknown_streams = 0;
int copy_unknown_streams = 0;
int recast_media = 0;

// this struct is passed as the optctx argument
// to func_arg() for global options
typedef struct GlobalOptionsContext {
    Scheduler      *sch;

    char          **filtergraphs;
    int          nb_filtergraphs;
} GlobalOptionsContext;

static void uninit_options(OptionsContext *o)
{
    /* all OPT_SPEC and OPT_TYPE_STRING can be freed in generic way */
    for (const OptionDef *po = options; po->name; po++) {
        void *dst;

        if (!(po->flags & OPT_FLAG_OFFSET))
            continue;

        dst = (uint8_t*)o + po->u.off;
        if (po->flags & OPT_FLAG_SPEC) {
            SpecifierOptList *so = dst;
            for (int i = 0; i < so->nb_opt; i++) {
                av_freep(&so->opt[i].specifier);
                if (po->flags & OPT_FLAG_PERSTREAM)
                    stream_specifier_uninit(&so->opt[i].stream_spec);
                if (po->type == OPT_TYPE_STRING)
                    av_freep(&so->opt[i].u.str);
            }
            av_freep(&so->opt);
            so->nb_opt = 0;
        } else if (po->type == OPT_TYPE_STRING)
            av_freep(dst);
    }

    for (int i = 0; i < o->nb_stream_maps; i++)
        av_freep(&o->stream_maps[i].linklabel);
    av_freep(&o->stream_maps);

    for (int i = 0; i < o->nb_attachments; i++)
        av_freep(&o->attachments[i]);
    av_freep(&o->attachments);

    av_dict_free(&o->streamid);
}

static void init_options(OptionsContext *o)
{
    memset(o, 0, sizeof(*o));

    o->stop_time = INT64_MAX;
    o->mux_max_delay  = 0.7;
    o->start_time     = AV_NOPTS_VALUE;
    o->start_time_eof = AV_NOPTS_VALUE;
    o->recording_time = INT64_MAX;
    o->limit_filesize = INT64_MAX;
    o->chapters_input_file = INT_MAX;
    o->accurate_seek  = 1;
    o->thread_queue_size = 0;
    o->input_sync_ref = -1;
    o->find_stream_info = 1;
    o->shortest_buf_duration = 10.f;
}

static int show_hwaccels(void *optctx, const char *opt, const char *arg)
{
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

    printf("Hardware acceleration methods:\n");
    while ((type = av_hwdevice_iterate_types(type)) !=
           AV_HWDEVICE_TYPE_NONE)
        printf("%s\n", av_hwdevice_get_type_name(type));
    printf("\n");
    return 0;
}

const char *opt_match_per_type_str(const SpecifierOptList *sol,
                                   char mediatype)
{
    av_assert0(!sol->nb_opt || sol->type == OPT_TYPE_STRING);

    for (int i = 0; i < sol->nb_opt; i++) {
        const char *spec = sol->opt[i].specifier;
        if (spec[0] == mediatype && !spec[1])
            return sol->opt[i].u.str;
    }
    return NULL;
}

static unsigned opt_match_per_stream(void *logctx, enum OptionType type,
                                     const SpecifierOptList *sol,
                                     AVFormatContext *fc, AVStream *st)
{
    int matches = 0, match_idx = -1;

    av_assert0((type == sol->type) || !sol->nb_opt);

    for (int i = 0; i < sol->nb_opt; i++) {
        const StreamSpecifier *ss = &sol->opt[i].stream_spec;

        if (stream_specifier_match(ss, fc, st, logctx)) {
            match_idx = i;
            matches++;
        }
    }

    if (matches > 1 && sol->opt_canon) {
        const SpecifierOpt *so = &sol->opt[match_idx];
        const char *spec = so->specifier && so->specifier[0] ? so->specifier : "";

        char namestr[128] = "";
        char optval_buf[32];
        const char *optval = optval_buf;

        snprintf(namestr, sizeof(namestr), "-%s", sol->opt_canon->name);
        if (sol->opt_canon->flags & OPT_HAS_ALT) {
            const char * const *names_alt = sol->opt_canon->u1.names_alt;
            for (int i = 0; names_alt[i]; i++)
                av_strlcatf(namestr, sizeof(namestr), "/-%s", names_alt[i]);
        }

        switch (sol->type) {
        case OPT_TYPE_STRING: optval = so->u.str;                                             break;
        case OPT_TYPE_INT:    snprintf(optval_buf, sizeof(optval_buf), "%d", so->u.i);        break;
        case OPT_TYPE_INT64:  snprintf(optval_buf, sizeof(optval_buf), "%"PRId64, so->u.i64); break;
        case OPT_TYPE_FLOAT:  snprintf(optval_buf, sizeof(optval_buf), "%f", so->u.f);        break;
        case OPT_TYPE_DOUBLE: snprintf(optval_buf, sizeof(optval_buf), "%f", so->u.dbl);      break;
        default: av_assert0(0);
        }

        av_log(logctx, AV_LOG_WARNING, "Multiple %s options specified for "
               "stream %d, only the last option '-%s%s%s %s' will be used.\n",
               namestr, st->index, sol->opt_canon->name, spec[0] ? ":" : "",
               spec, optval);
    }

    return match_idx + 1;
}

#define OPT_MATCH_PER_STREAM(name, type, opt_type, m)                                   \
void opt_match_per_stream_ ## name(void *logctx, const SpecifierOptList *sol,           \
                                   AVFormatContext *fc, AVStream *st, type *out)        \
{                                                                                       \
    unsigned ret = opt_match_per_stream(logctx, opt_type, sol, fc, st);                 \
    if (ret > 0)                                                                        \
        *out = sol->opt[ret - 1].u.m;                                                   \
}

OPT_MATCH_PER_STREAM(str,   const char *, OPT_TYPE_STRING, str);
OPT_MATCH_PER_STREAM(int,   int,          OPT_TYPE_INT,    i);
OPT_MATCH_PER_STREAM(int64, int64_t,      OPT_TYPE_INT64,  i64);
OPT_MATCH_PER_STREAM(dbl,   double,       OPT_TYPE_DOUBLE, dbl);

static unsigned opt_match_per_stream_group(void *logctx, enum OptionType type,
                                           const SpecifierOptList *sol,
                                           AVFormatContext *fc, AVStreamGroup *stg)
{
    int matches = 0, match_idx = -1;

    av_assert0((type == sol->type) || !sol->nb_opt);

    for (int i = 0; i < sol->nb_opt; i++) {
        const StreamSpecifier *ss = &sol->opt[i].stream_spec;

        if (stream_group_specifier_match(ss, fc, stg, logctx)) {
            match_idx = i;
            matches++;
        }
    }

    if (matches > 1 && sol->opt_canon) {
        const SpecifierOpt *so = &sol->opt[match_idx];
        const char *spec = so->specifier && so->specifier[0] ? so->specifier : "";

        char namestr[128] = "";
        char optval_buf[32];
        const char *optval = optval_buf;

        snprintf(namestr, sizeof(namestr), "-%s", sol->opt_canon->name);
        if (sol->opt_canon->flags & OPT_HAS_ALT) {
            const char * const *names_alt = sol->opt_canon->u1.names_alt;
            for (int i = 0; names_alt[i]; i++)
                av_strlcatf(namestr, sizeof(namestr), "/-%s", names_alt[i]);
        }

        switch (sol->type) {
        case OPT_TYPE_STRING: optval = so->u.str;                                             break;
        case OPT_TYPE_INT:    snprintf(optval_buf, sizeof(optval_buf), "%d", so->u.i);        break;
        case OPT_TYPE_INT64:  snprintf(optval_buf, sizeof(optval_buf), "%"PRId64, so->u.i64); break;
        case OPT_TYPE_FLOAT:  snprintf(optval_buf, sizeof(optval_buf), "%f", so->u.f);        break;
        case OPT_TYPE_DOUBLE: snprintf(optval_buf, sizeof(optval_buf), "%f", so->u.dbl);      break;
        default: av_assert0(0);
        }

        av_log(logctx, AV_LOG_WARNING, "Multiple %s options specified for "
               "stream group %d, only the last option '-%s%s%s %s' will be used.\n",
               namestr, stg->index, sol->opt_canon->name, spec[0] ? ":" : "",
               spec, optval);
    }

    return match_idx + 1;
}

#define OPT_MATCH_PER_STREAM_GROUP(name, type, opt_type, m)                                  \
void opt_match_per_stream_group_ ## name(void *logctx, const SpecifierOptList *sol,          \
                                         AVFormatContext *fc, AVStreamGroup *stg, type *out) \
{                                                                                            \
    unsigned ret = opt_match_per_stream_group(logctx, opt_type, sol, fc, stg);               \
    if (ret > 0)                                                                             \
        *out = sol->opt[ret - 1].u.m;                                                        \
}

OPT_MATCH_PER_STREAM_GROUP(str,   const char *, OPT_TYPE_STRING, str);
OPT_MATCH_PER_STREAM_GROUP(int,   int,          OPT_TYPE_INT,    i);
OPT_MATCH_PER_STREAM_GROUP(int64, int64_t,      OPT_TYPE_INT64,  i64);
OPT_MATCH_PER_STREAM_GROUP(dbl,   double,       OPT_TYPE_DOUBLE, dbl);

int view_specifier_parse(const char **pspec, ViewSpecifier *vs)
{
    const char *spec = *pspec;
    char *endptr;

    vs->type = VIEW_SPECIFIER_TYPE_NONE;

    if (!strncmp(spec, "view:", 5)) {
        spec += 5;

        if (!strncmp(spec, "all", 3)) {
            spec += 3;
            vs->type = VIEW_SPECIFIER_TYPE_ALL;
        } else {
            vs->type = VIEW_SPECIFIER_TYPE_ID;
            vs->val  = strtoul(spec, &endptr, 0);
            if (endptr == spec) {
                av_log(NULL, AV_LOG_ERROR, "Invalid view ID: %s\n", spec);
                return AVERROR(EINVAL);
            }
            spec = endptr;
        }
    } else if (!strncmp(spec, "vidx:", 5)) {
        spec += 5;
        vs->type = VIEW_SPECIFIER_TYPE_IDX;
        vs->val  = strtoul(spec, &endptr, 0);
        if (endptr == spec) {
            av_log(NULL, AV_LOG_ERROR, "Invalid view index: %s\n", spec);
            return AVERROR(EINVAL);
        }
        spec = endptr;
    } else if (!strncmp(spec, "vpos:", 5)) {
        spec += 5;
        vs->type = VIEW_SPECIFIER_TYPE_POS;

        if (!strncmp(spec, "left", 4) && !cmdutils_isalnum(spec[4])) {
            spec += 4;
            vs->val = AV_STEREO3D_VIEW_LEFT;
        } else if (!strncmp(spec, "right", 5) && !cmdutils_isalnum(spec[5])) {
            spec += 5;
            vs->val = AV_STEREO3D_VIEW_RIGHT;
        } else {
            av_log(NULL, AV_LOG_ERROR, "Invalid view position: %s\n", spec);
            return AVERROR(EINVAL);
        }
    } else
        return 0;

    *pspec = spec;

    return 0;
}

int parse_and_set_vsync(const char *arg, enum VideoSyncMethod *vsync_var, int file_idx, int st_idx, int is_global)
{
    if      (!av_strcasecmp(arg, "cfr"))         *vsync_var = VSYNC_CFR;
    else if (!av_strcasecmp(arg, "vfr"))         *vsync_var = VSYNC_VFR;
    else if (!av_strcasecmp(arg, "passthrough")) *vsync_var = VSYNC_PASSTHROUGH;
#if FFMPEG_OPT_VSYNC_DROP
    else if (!av_strcasecmp(arg, "drop")) {
        av_log(NULL, AV_LOG_WARNING, "-vsync/fps_mode drop is deprecated\n");
        *vsync_var = VSYNC_DROP;
    }
#endif
    else if (!is_global && !av_strcasecmp(arg, "auto"))  *vsync_var = VSYNC_AUTO;
    else if (!is_global) {
        av_log(NULL, AV_LOG_FATAL, "Invalid value %s specified for fps_mode of #%d:%d.\n", arg, file_idx, st_idx);
        return AVERROR(EINVAL);
    }

#if FFMPEG_OPT_VSYNC
    if (is_global && *vsync_var == VSYNC_AUTO) {
        int ret;
        double num;

        ret = parse_number("vsync", arg, OPT_TYPE_INT, VSYNC_AUTO, VSYNC_VFR, &num);
        if (ret < 0)
            return ret;

        video_sync_method = num;
        av_log(NULL, AV_LOG_WARNING, "Passing a number to -vsync is deprecated,"
               " use a string argument as described in the manual.\n");
    }
#endif

    return 0;
}

/* Correct input file start times based on enabled streams */
static void correct_input_start_times(void)
{
    for (int i = 0; i < nb_input_files; i++) {
        InputFile       *ifile = input_files[i];
        AVFormatContext    *is = ifile->ctx;
        int64_t new_start_time = INT64_MAX, diff, abs_start_seek;

        ifile->start_time_effective = is->start_time;

        if (is->start_time == AV_NOPTS_VALUE ||
            !(is->iformat->flags & AVFMT_TS_DISCONT))
            continue;

        for (int j = 0; j < is->nb_streams; j++) {
            AVStream *st = is->streams[j];
            if(st->discard == AVDISCARD_ALL || st->start_time == AV_NOPTS_VALUE)
                continue;
            new_start_time = FFMIN(new_start_time, av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q));
        }

        diff = new_start_time - is->start_time;
        if (diff) {
            av_log(NULL, AV_LOG_VERBOSE, "Correcting start time of Input #%d by %"PRId64" us.\n", i, diff);
            ifile->start_time_effective = new_start_time;
            if (copy_ts && start_at_zero)
                ifile->ts_offset = -new_start_time;
            else if (!copy_ts) {
                abs_start_seek = is->start_time + ((ifile->start_time != AV_NOPTS_VALUE) ? ifile->start_time : 0);
                ifile->ts_offset = abs_start_seek > new_start_time ? -abs_start_seek : -new_start_time;
            } else if (copy_ts)
                ifile->ts_offset = 0;

            ifile->ts_offset += ifile->input_ts_offset;
        }
    }
}

static int apply_sync_offsets(void)
{
    for (int i = 0; i < nb_input_files; i++) {
        InputFile *ref, *self = input_files[i];
        int64_t adjustment;
        int64_t self_start_time, ref_start_time, self_seek_start, ref_seek_start;
        int start_times_set = 1;

        if (self->input_sync_ref == -1 || self->input_sync_ref == i) continue;
        if (self->input_sync_ref >= nb_input_files || self->input_sync_ref < -1) {
            av_log(NULL, AV_LOG_FATAL, "-isync for input %d references non-existent input %d.\n", i, self->input_sync_ref);
            return AVERROR(EINVAL);
        }

        if (copy_ts && !start_at_zero) {
            av_log(NULL, AV_LOG_FATAL, "Use of -isync requires that start_at_zero be set if copyts is set.\n");
            return AVERROR(EINVAL);
        }

        ref = input_files[self->input_sync_ref];
        if (ref->input_sync_ref != -1 && ref->input_sync_ref != self->input_sync_ref) {
            av_log(NULL, AV_LOG_ERROR, "-isync for input %d references a resynced input %d. Sync not set.\n", i, self->input_sync_ref);
            continue;
        }

        if (self->ctx->start_time_realtime != AV_NOPTS_VALUE && ref->ctx->start_time_realtime != AV_NOPTS_VALUE) {
            self_start_time = self->ctx->start_time_realtime;
            ref_start_time  =  ref->ctx->start_time_realtime;
        } else if (self->start_time_effective != AV_NOPTS_VALUE && ref->start_time_effective != AV_NOPTS_VALUE) {
            self_start_time = self->start_time_effective;
            ref_start_time  =  ref->start_time_effective;
        } else {
            start_times_set = 0;
        }

        if (start_times_set) {
            self_seek_start = self->start_time == AV_NOPTS_VALUE ? 0 : self->start_time;
            ref_seek_start  =  ref->start_time == AV_NOPTS_VALUE ? 0 :  ref->start_time;

            adjustment = (self_start_time - ref_start_time) + !copy_ts*(self_seek_start - ref_seek_start) + ref->input_ts_offset;

            self->ts_offset += adjustment;

            av_log(NULL, AV_LOG_INFO, "Adjusted ts offset for Input #%d by %"PRId64" us to sync with Input #%d.\n", i, adjustment, self->input_sync_ref);
        } else {
            av_log(NULL, AV_LOG_INFO, "Unable to identify start times for Inputs #%d and %d both. No sync adjustment made.\n", i, self->input_sync_ref);
        }
    }

    return 0;
}

static int opt_filter_threads(void *optctx, const char *opt, const char *arg)
{
    av_free(filter_nbthreads);
    filter_nbthreads = av_strdup(arg);
    return 0;
}

static int opt_abort_on(void *optctx, const char *opt, const char *arg)
{
    static const AVOption opts[] = {
        { "abort_on"           , NULL, 0, AV_OPT_TYPE_FLAGS, { .i64 = 0 }, INT64_MIN, (double)INT64_MAX,   .unit = "flags" },
        { "empty_output"       , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = ABORT_ON_FLAG_EMPTY_OUTPUT        }, .unit = "flags" },
        { "empty_output_stream", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = ABORT_ON_FLAG_EMPTY_OUTPUT_STREAM }, .unit = "flags" },
        { NULL },
    };
    static const AVClass class = {
        .class_name = "",
        .item_name  = av_default_item_name,
        .option     = opts,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *pclass = &class;

    return av_opt_eval_flags(&pclass, &opts[0], arg, &abort_on_flags);
}

static int opt_stats_period(void *optctx, const char *opt, const char *arg)
{
    int64_t user_stats_period;
    int ret = av_parse_time(&user_stats_period, arg, 1);
    if (ret < 0)
        return ret;

    if (user_stats_period <= 0) {
        av_log(NULL, AV_LOG_ERROR, "stats_period %s must be positive.\n", arg);
        return AVERROR(EINVAL);
    }

    stats_period = user_stats_period;
    av_log(NULL, AV_LOG_INFO, "ffmpeg stats and -progress period set to %s.\n", arg);

    return 0;
}

static int opt_audio_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:a", arg, options);
}

static int opt_video_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:v", arg, options);
}

static int opt_subtitle_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:s", arg, options);
}

static int opt_data_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:d", arg, options);
}

static int opt_map(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    StreamMap *m = NULL;
    StreamSpecifier ss;
    int i, negative = 0, file_idx, disabled = 0;
    int ret, allow_unused = 0;

    memset(&ss, 0, sizeof(ss));

    if (*arg == '-') {
        negative = 1;
        arg++;
    }

    if (arg[0] == '[') {
        ViewSpecifier vs;
        /* this mapping refers to lavfi output */
        const char *c = arg + 1;
        char *endptr;

        ret = GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
        if (ret < 0)
            goto fail;

        m = &o->stream_maps[o->nb_stream_maps - 1];
        m->linklabel = av_get_token(&c, "]");
        if (!m->linklabel) {
            av_log(NULL, AV_LOG_ERROR, "Invalid output link label: %s.\n", arg);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        arg++;

        m->group_index = -1;
        file_idx = strtol(arg, &endptr, 0);
        if (file_idx >= nb_input_files || file_idx < 0)
            goto end;

        arg = endptr;
        ret = stream_specifier_parse(&ss, *arg == ':' ? arg + 1 : arg, 1, NULL);
        if (ret < 0)
            goto end;

        arg = ss.remainder ? ss.remainder : "";
        ret = view_specifier_parse(&arg, &vs);
        if (ret < 0 || (*arg && strcmp(arg, "]")))
            goto end;

        m->file_index  = file_idx;
        m->stream_index = ss.idx;
        m->group_index = ss.stream_list == STREAM_LIST_GROUP_IDX ? ss.list_id : -1;
    } else {
        ViewSpecifier vs;
        char *endptr;

        file_idx = strtol(arg, &endptr, 0);
        if (file_idx >= nb_input_files || file_idx < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index: %d.\n", file_idx);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        arg = endptr;

        ret = stream_specifier_parse(&ss, *arg == ':' ? arg + 1 : arg, 1, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Invalid stream specifier: %s\n", arg);
            goto fail;
        }

        arg = ss.remainder ? ss.remainder : "";

        ret = view_specifier_parse(&arg, &vs);
        if (ret < 0)
            goto fail;

        if (*arg) {
            if (!strcmp(arg, "?"))
                allow_unused = 1;
            else {
                av_log(NULL, AV_LOG_ERROR,
                       "Trailing garbage after stream specifier: %s\n", arg);
                ret = AVERROR(EINVAL);
                goto fail;
            }
        }

        if (negative)
            /* disable some already defined maps */
            for (i = 0; i < o->nb_stream_maps; i++) {
                m = &o->stream_maps[i];
                if (file_idx == m->file_index &&
                    stream_specifier_match(&ss,
                                           input_files[m->file_index]->ctx,
                                           input_files[m->file_index]->ctx->streams[m->stream_index],
                                           NULL))
                    m->disabled = 1;
            }
        else
            for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
                if (!stream_specifier_match(&ss,
                                            input_files[file_idx]->ctx,
                                            input_files[file_idx]->ctx->streams[i],
                                            NULL))
                    continue;
                if (input_files[file_idx]->streams[i]->user_set_discard == AVDISCARD_ALL) {
                    disabled = 1;
                    continue;
                }
                ret = GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
                if (ret < 0)
                    goto fail;

                m = &o->stream_maps[o->nb_stream_maps - 1];

                m->file_index   = file_idx;
                m->stream_index = i;
                m->group_index  = ss.stream_list == STREAM_LIST_GROUP_IDX ? ss.list_id : -1;
                m->vs           = vs;
            }
    }

    if (!m) {
        if (allow_unused) {
            av_log(NULL, AV_LOG_VERBOSE, "Stream map '%s' matches no streams; ignoring.\n", arg);
        } else if (disabled) {
            av_log(NULL, AV_LOG_FATAL, "Stream map '%s' matches disabled streams.\n"
                                       "To ignore this, add a trailing '?' to the map.\n", arg);
            ret = AVERROR(EINVAL);
            goto fail;
        } else {
            av_log(NULL, AV_LOG_FATAL, "Stream map '%s' matches no streams.\n"
                                       "To ignore this, add a trailing '?' to the map.\n", arg);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
end:
    ret = 0;
fail:
    stream_specifier_uninit(&ss);
    return ret;
}

static int opt_attach(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    int ret = GROW_ARRAY(o->attachments, o->nb_attachments);
    if (ret < 0)
        return ret;

    o->attachments[o->nb_attachments - 1] = av_strdup(arg);
    if (!o->attachments[o->nb_attachments - 1])
        return AVERROR(ENOMEM);

    return 0;
}

static int opt_sdp_file(void *optctx, const char *opt, const char *arg)
{
    GlobalOptionsContext *go = optctx;
    return sch_sdp_filename(go->sch, arg);
}

#if CONFIG_VAAPI
static int opt_vaapi_device(void *optctx, const char *opt, const char *arg)
{
    const char *prefix = "vaapi:";
    char *tmp;
    int err;
    tmp = av_asprintf("%s%s", prefix, arg);
    if (!tmp)
        return AVERROR(ENOMEM);
    err = hw_device_init_from_string(tmp, NULL);
    av_free(tmp);
    return err;
}
#endif

#if CONFIG_QSV
static int opt_qsv_device(void *optctx, const char *opt, const char *arg)
{
    const char *prefix = "qsv=__qsv_device:hw_any,child_device=";
    int err;
    char *tmp = av_asprintf("%s%s", prefix, arg);

    if (!tmp)
        return AVERROR(ENOMEM);

    err = hw_device_init_from_string(tmp, NULL);
    av_free(tmp);

    return err;
}
#endif

static int opt_init_hw_device(void *optctx, const char *opt, const char *arg)
{
    if (!strcmp(arg, "list")) {
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        printf("Supported hardware device types:\n");
        while ((type = av_hwdevice_iterate_types(type)) !=
               AV_HWDEVICE_TYPE_NONE)
            printf("%s\n", av_hwdevice_get_type_name(type));
        printf("\n");
        return AVERROR_EXIT;
    } else {
        return hw_device_init_from_string(arg, NULL);
    }
}

static int opt_filter_hw_device(void *optctx, const char *opt, const char *arg)
{
    if (filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Only one filter device can be used.\n");
        return AVERROR(EINVAL);
    }
    filter_hw_device = hw_device_get_by_name(arg);
    if (!filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Invalid filter device %s.\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_recording_timestamp(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char buf[128];
    int64_t recording_timestamp;
    int ret;
    struct tm time;

    ret = av_parse_time(&recording_timestamp, arg, 0);
    if (ret < 0)
        return ret;

    recording_timestamp /= 1e6;
    time = *gmtime((time_t*)&recording_timestamp);
    if (!strftime(buf, sizeof(buf), "creation_time=%Y-%m-%dT%H:%M:%S%z", &time))
        return -1;
    parse_option(o, "metadata", buf, options);

    av_log(NULL, AV_LOG_WARNING, "%s is deprecated, set the 'creation_time' metadata "
                                 "tag instead.\n", opt);
    return 0;
}

int find_codec(void *logctx, const char *name,
               enum AVMediaType type, int encoder, const AVCodec **pcodec)
{
    const AVCodecDescriptor *desc;
    const char *codec_string = encoder ? "encoder" : "decoder";
    const AVCodec *codec;

    codec = encoder ?
        avcodec_find_encoder_by_name(name) :
        avcodec_find_decoder_by_name(name);

    if (!codec && (desc = avcodec_descriptor_get_by_name(name))) {
        codec = encoder ? avcodec_find_encoder(desc->id) :
                          avcodec_find_decoder(desc->id);
        if (codec)
            av_log(logctx, AV_LOG_VERBOSE, "Matched %s '%s' for codec '%s'.\n",
                   codec_string, codec->name, desc->name);
    }

    if (!codec) {
        av_log(logctx, AV_LOG_FATAL, "Unknown %s '%s'\n", codec_string, name);
        return encoder ? AVERROR_ENCODER_NOT_FOUND :
                         AVERROR_DECODER_NOT_FOUND;
    }
    if (codec->type != type && !recast_media) {
        av_log(logctx, AV_LOG_FATAL, "Invalid %s type '%s'\n", codec_string, name);
        return AVERROR(EINVAL);
    }

    *pcodec = codec;
    return 0;;
}

int assert_file_overwrite(const char *filename)
{
    const char *proto_name = avio_find_protocol_name(filename);

    if (file_overwrite && no_file_overwrite) {
        fprintf(stderr, "Error, both -y and -n supplied. Exiting.\n");
        return AVERROR(EINVAL);
    }

    if (!file_overwrite) {
        if (proto_name && !strcmp(proto_name, "file") && avio_check(filename, 0) == 0) {
            if (stdin_interaction && !no_file_overwrite) {
                fprintf(stderr,"File '%s' already exists. Overwrite? [y/N] ", filename);
                fflush(stderr);
                term_exit();
                signal(SIGINT, SIG_DFL);
                if (!read_yesno()) {
                    av_log(NULL, AV_LOG_FATAL, "Not overwriting - exiting\n");
                    return AVERROR_EXIT;
                }
                term_init();
            }
            else {
                av_log(NULL, AV_LOG_FATAL, "File '%s' already exists. Exiting.\n", filename);
                return AVERROR_EXIT;
            }
        }
    }

    if (proto_name && !strcmp(proto_name, "file")) {
        for (int i = 0; i < nb_input_files; i++) {
             InputFile *file = input_files[i];
             if (file->ctx->iformat->flags & AVFMT_NOFILE)
                 continue;
             if (!strcmp(filename, file->ctx->url)) {
                 av_log(NULL, AV_LOG_FATAL, "Output %s same as Input #%d - exiting\n", filename, i);
                 av_log(NULL, AV_LOG_WARNING, "FFmpeg cannot edit existing files in-place.\n");
                 return AVERROR(EINVAL);
             }
        }
    }

    return 0;
}

/* arg format is "output-stream-index:streamid-value". */
static int opt_streamid(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char *p;
    char idx_str[16];

    av_strlcpy(idx_str, arg, sizeof(idx_str));
    p = strchr(idx_str, ':');
    if (!p) {
        av_log(NULL, AV_LOG_FATAL,
               "Invalid value '%s' for option '%s', required syntax is 'index:value'\n",
               arg, opt);
        return AVERROR(EINVAL);
    }
    *p++ = '\0';

    return av_dict_set(&o->streamid, idx_str, p, 0);
}

static int opt_target(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    enum { PAL, NTSC, FILM, UNKNOWN } norm = UNKNOWN;
    static const char *const frame_rates[] = { "25", "30000/1001", "24000/1001" };

    if (!strncmp(arg, "pal-", 4)) {
        norm = PAL;
        arg += 4;
    } else if (!strncmp(arg, "ntsc-", 5)) {
        norm = NTSC;
        arg += 5;
    } else if (!strncmp(arg, "film-", 5)) {
        norm = FILM;
        arg += 5;
    } else {
        /* Try to determine PAL/NTSC by peeking in the input files */
        if (nb_input_files) {
            int i, j;
            for (j = 0; j < nb_input_files; j++) {
                for (i = 0; i < input_files[j]->nb_streams; i++) {
                    AVStream *st = input_files[j]->ctx->streams[i];
                    int64_t fr;
                    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                        continue;
                    fr = st->time_base.den * 1000LL / st->time_base.num;
                    if (fr == 25000) {
                        norm = PAL;
                        break;
                    } else if ((fr == 29970) || (fr == 23976)) {
                        norm = NTSC;
                        break;
                    }
                }
                if (norm != UNKNOWN)
                    break;
            }
        }
        if (norm != UNKNOWN)
            av_log(NULL, AV_LOG_INFO, "Assuming %s for target.\n", norm == PAL ? "PAL" : "NTSC");
    }

    if (norm == UNKNOWN) {
        av_log(NULL, AV_LOG_FATAL, "Could not determine norm (PAL/NTSC/NTSC-Film) for target.\n");
        av_log(NULL, AV_LOG_FATAL, "Please prefix target with \"pal-\", \"ntsc-\" or \"film-\",\n");
        av_log(NULL, AV_LOG_FATAL, "or set a framerate with \"-r xxx\".\n");
        return AVERROR(EINVAL);
    }

    if (!strcmp(arg, "vcd")) {
        opt_video_codec(o, "c:v", "mpeg1video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "vcd", options);

        parse_option(o, "s", norm == PAL ? "352x288" : "352x240", options);
        parse_option(o, "r", frame_rates[norm], options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "1150000");
        opt_default(NULL, "maxrate:v", "1150000");
        opt_default(NULL, "minrate:v", "1150000");
        opt_default(NULL, "bufsize:v", "327680"); // 40*1024*8;

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);
        parse_option(o, "ac", "2", options);

        opt_default(NULL, "packetsize", "2324");
        opt_default(NULL, "muxrate", "1411200"); // 2352 * 75 * 8;

        /* We have to offset the PTS, so that it is consistent with the SCR.
           SCR starts at 36000, but the first two packs contain only padding
           and the first pack from the other stream, respectively, may also have
           been written before.
           So the real data starts at SCR 36000+3*1200. */
        o->mux_preload = (36000 + 3 * 1200) / 90000.0; // 0.44
    } else if (!strcmp(arg, "svcd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "svcd", options);

        parse_option(o, "s", norm == PAL ? "480x576" : "480x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "2040000");
        opt_default(NULL, "maxrate:v", "2516000");
        opt_default(NULL, "minrate:v", "0"); // 1145000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;
        opt_default(NULL, "scan_offset", "1");

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);

        opt_default(NULL, "packetsize", "2324");

    } else if (!strcmp(arg, "dvd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "ac3");
        parse_option(o, "f", "dvd", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "6000000");
        opt_default(NULL, "maxrate:v", "9000000");
        opt_default(NULL, "minrate:v", "0"); // 1500000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;

        opt_default(NULL, "packetsize", "2048");  // from www.mpucoder.com: DVD sectors contain 2048 bytes of data, this is also the size of one pack.
        opt_default(NULL, "muxrate", "10080000"); // from mplex project: data_rate = 1260000. mux_rate = data_rate * 8

        opt_default(NULL, "b:a", "448000");
        parse_option(o, "ar", "48000", options);

    } else if (!strncmp(arg, "dv", 2)) {

        parse_option(o, "f", "dv", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "pix_fmt", !strncmp(arg, "dv50", 4) ? "yuv422p" :
                          norm == PAL ? "yuv420p" : "yuv411p", options);
        parse_option(o, "r", frame_rates[norm], options);

        parse_option(o, "ar", "48000", options);
        parse_option(o, "ac", "2", options);

    } else {
        av_log(NULL, AV_LOG_ERROR, "Unknown target: %s\n", arg);
        return AVERROR(EINVAL);
    }

    av_dict_copy(&o->g->codec_opts,  codec_opts, AV_DICT_DONT_OVERWRITE);
    av_dict_copy(&o->g->format_opts, format_opts, AV_DICT_DONT_OVERWRITE);

    return 0;
}

static int opt_vstats_file(void *optctx, const char *opt, const char *arg)
{
    av_free (vstats_filename);
    vstats_filename = av_strdup (arg);
    return 0;
}

static int opt_vstats(void *optctx, const char *opt, const char *arg)
{
    char filename[40];
    time_t today2 = time(NULL);
    struct tm *today = localtime(&today2);

    if (!today) { // maybe tomorrow
        av_log(NULL, AV_LOG_FATAL, "Unable to get current time: %s\n", strerror(errno));
        return AVERROR(errno);
    }

    snprintf(filename, sizeof(filename), "vstats_%02d%02d%02d.log", today->tm_hour, today->tm_min,
             today->tm_sec);
    return opt_vstats_file(NULL, opt, filename);
}

static int opt_video_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:v", arg, options);
}

static int opt_audio_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:a", arg, options);
}

static int opt_data_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:d", arg, options);
}

static int opt_default_new(OptionsContext *o, const char *opt, const char *arg)
{
    int ret;
    AVDictionary *cbak = codec_opts;
    AVDictionary *fbak = format_opts;
    codec_opts = NULL;
    format_opts = NULL;

    ret = opt_default(NULL, opt, arg);

    av_dict_copy(&o->g->codec_opts , codec_opts, 0);
    av_dict_copy(&o->g->format_opts, format_opts, 0);
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);
    codec_opts = cbak;
    format_opts = fbak;

    return ret;
}

static int opt_preset(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    FILE *f=NULL;
    char filename[1000], line[1000], tmp_line[1000];
    const char *codec_name = NULL;
    int ret = 0;
    int depth = o->depth;

    if (depth > 2) {
        av_log(NULL, AV_LOG_ERROR, "too deep recursion\n");
        return AVERROR(EINVAL);
    }

    codec_name = opt_match_per_type_str(&o->codec_names, *opt);

    if (!(f = get_preset_file(filename, sizeof(filename), arg, *opt == 'f', codec_name))) {
        if(!strncmp(arg, "libx264-lossless", strlen("libx264-lossless"))){
            av_log(NULL, AV_LOG_FATAL, "Please use -preset <speed> -qp 0\n");
        }else
            av_log(NULL, AV_LOG_FATAL, "File for preset '%s' not found\n", arg);
        return AVERROR(ENOENT);
    }

    o->depth ++;
    while (fgets(line, sizeof(line), f)) {
        char *key = tmp_line, *value, *endptr;

        if (strcspn(line, "#\n\r") == 0)
            continue;
        av_strlcpy(tmp_line, line, sizeof(tmp_line));
        if (!av_strtok(key,   "=",    &value) ||
            !av_strtok(value, "\r\n", &endptr)) {
            av_log(NULL, AV_LOG_FATAL, "%s: Invalid syntax: '%s'\n", filename, line);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        av_log(NULL, AV_LOG_DEBUG, "ffpreset[%s]: set '%s' = '%s'\n", filename, key, value);

        if      (!strcmp(key, "acodec")) opt_audio_codec   (o, key, value);
        else if (!strcmp(key, "vcodec")) opt_video_codec   (o, key, value);
        else if (!strcmp(key, "scodec")) opt_subtitle_codec(o, key, value);
        else if (!strcmp(key, "dcodec")) opt_data_codec    (o, key, value);
        else if ((parse_option(o, key, value, options) < 0) &&
                 (opt_default_new(o, key, value) < 0)) {
            av_log(NULL, AV_LOG_FATAL, "%s: Invalid option or argument: '%s', parsed as '%s' = '%s'\n",
                   filename, line, key, value);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

fail:
    o->depth = depth;
    fclose(f);

    return ret;
}

static int opt_old2new(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    int ret;
    char *s = av_asprintf("%s:%c", opt + 1, *opt);
    if (!s)
        return AVERROR(ENOMEM);
    ret = parse_option(o, s, arg, options);
    av_free(s);
    return ret;
}

static int opt_bitrate(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;

    if(!strcmp(opt, "ab")){
        av_dict_set(&o->g->codec_opts, "b:a", arg, 0);
        return 0;
    } else if(!strcmp(opt, "b")){
        av_log(NULL, AV_LOG_WARNING, "Please use -b:a or -b:v, -b is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "b:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

static int opt_qscale(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char *s;
    int ret;
    if(!strcmp(opt, "qscale")){
        av_log(NULL, AV_LOG_WARNING, "Please use -q:a or -q:v, -qscale is ambiguous\n");
        return parse_option(o, "q:v", arg, options);
    }
    s = av_asprintf("q%s", opt + 6);
    if (!s)
        return AVERROR(ENOMEM);
    ret = parse_option(o, s, arg, options);
    av_free(s);
    return ret;
}

static int opt_profile(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    if(!strcmp(opt, "profile")){
        av_log(NULL, AV_LOG_WARNING, "Please use -profile:a or -profile:v, -profile is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "profile:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

static int opt_video_filters(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "filter:v", arg, options);
}

static int opt_audio_filters(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "filter:a", arg, options);
}

#if FFMPEG_OPT_VSYNC
static int opt_vsync(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "-vsync is deprecated. Use -fps_mode\n");
    return parse_and_set_vsync(arg, &video_sync_method, -1, -1, 1);
}
#endif

static int opt_timecode(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    int ret;
    char *tcr = av_asprintf("timecode=%s", arg);
    if (!tcr)
        return AVERROR(ENOMEM);
    ret = parse_option(o, "metadata:g", tcr, options);
    if (ret >= 0)
        ret = av_dict_set(&o->g->codec_opts, "gop_timecode", arg, 0);
    av_free(tcr);
    return ret;
}

static int opt_audio_qscale(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "q:a", arg, options);
}

static int opt_filter_complex(void *optctx, const char *opt, const char *arg)
{
    GlobalOptionsContext *go = optctx;
    char *graph_desc;
    int ret;

    graph_desc = av_strdup(arg);
    if (!graph_desc)
        return AVERROR(ENOMEM);

    ret = GROW_ARRAY(go->filtergraphs, go->nb_filtergraphs);
    if (ret < 0) {
        av_freep(&graph_desc);
        return ret;
    }
    go->filtergraphs[go->nb_filtergraphs - 1] = graph_desc;

    return 0;
}

#if FFMPEG_OPT_FILTER_SCRIPT
static int opt_filter_complex_script(void *optctx, const char *opt, const char *arg)
{
    GlobalOptionsContext *go = optctx;
    char *graph_desc;
    int ret;

    graph_desc = read_file_to_string(arg);
    if (!graph_desc)
        return AVERROR(EINVAL);

    av_log(NULL, AV_LOG_WARNING, "-%s is deprecated, use -/filter_complex %s instead\n",
           opt, arg);

    ret = GROW_ARRAY(go->filtergraphs, go->nb_filtergraphs);
    if (ret < 0) {
        av_freep(&graph_desc);
        return ret;
    }
    go->filtergraphs[go->nb_filtergraphs - 1] = graph_desc;

    return 0;
}
#endif

void show_help_default(const char *opt, const char *arg)
{
    int show_advanced = 0, show_avoptions = 0;

    if (opt && *opt) {
        if (!strcmp(opt, "long"))
            show_advanced = 1;
        else if (!strcmp(opt, "full"))
            show_advanced = show_avoptions = 1;
        else
            av_log(NULL, AV_LOG_ERROR, "Unknown help option '%s'.\n", opt);
    }

    show_usage();

    printf("Getting help:\n"
           "    -h      -- print basic options\n"
           "    -h long -- print more options\n"
           "    -h full -- print all options (including all format and codec specific options, very long)\n"
           "    -h type=name -- print all options for the named decoder/encoder/demuxer/muxer/filter/bsf/protocol\n"
           "    See man %s for detailed description of the options.\n"
           "\n"
           "Per-stream options can be followed by :<stream_spec> to apply that option to specific streams only. "
           "<stream_spec> can be a stream index, or v/a/s for video/audio/subtitle (see manual for full syntax).\n"
           "\n", program_name);

    show_help_options(options, "Print help / information / capabilities:",
                      OPT_EXIT, OPT_EXPERT);
    if (show_advanced)
        show_help_options(options, "Advanced information / capabilities:",
                          OPT_EXIT | OPT_EXPERT, 0);

    show_help_options(options, "Global options (affect whole program "
                      "instead of just one file):",
                      0, OPT_PERFILE | OPT_EXIT | OPT_EXPERT);
    if (show_advanced)
        show_help_options(options, "Advanced global options:", OPT_EXPERT,
                          OPT_PERFILE | OPT_EXIT);

    show_help_options(options, "Per-file options (input and output):",
                      OPT_PERFILE | OPT_INPUT | OPT_OUTPUT,
                      OPT_EXIT | OPT_FLAG_PERSTREAM | OPT_EXPERT |
                      OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced per-file options (input and output):",
                          OPT_PERFILE | OPT_INPUT | OPT_OUTPUT | OPT_EXPERT,
                          OPT_EXIT | OPT_FLAG_PERSTREAM |
                          OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Per-file options (input-only):",
                      OPT_PERFILE | OPT_INPUT,
                      OPT_EXIT | OPT_FLAG_PERSTREAM | OPT_OUTPUT | OPT_EXPERT |
                      OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced per-file options (input-only):",
                          OPT_PERFILE | OPT_INPUT | OPT_EXPERT,
                          OPT_EXIT | OPT_FLAG_PERSTREAM | OPT_OUTPUT |
                          OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Per-file options (output-only):",
                      OPT_PERFILE | OPT_OUTPUT,
                      OPT_EXIT | OPT_FLAG_PERSTREAM | OPT_INPUT | OPT_EXPERT |
                      OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced per-file options (output-only):",
                          OPT_PERFILE | OPT_OUTPUT | OPT_EXPERT,
                          OPT_EXIT | OPT_FLAG_PERSTREAM | OPT_INPUT |
                          OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Per-stream options:",
                      OPT_FLAG_PERSTREAM,
                      OPT_EXIT | OPT_EXPERT |
                      OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced per-stream options:",
                          OPT_FLAG_PERSTREAM | OPT_EXPERT,
                          OPT_EXIT |
                          OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Video options:",
                      OPT_VIDEO, OPT_EXPERT | OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced Video options:",
                          OPT_EXPERT | OPT_VIDEO, OPT_AUDIO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Audio options:",
                      OPT_AUDIO, OPT_EXPERT | OPT_VIDEO | OPT_SUBTITLE | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced Audio options:",
                          OPT_EXPERT | OPT_AUDIO, OPT_VIDEO | OPT_SUBTITLE | OPT_DATA);

    show_help_options(options, "Subtitle options:",
                      OPT_SUBTITLE, OPT_EXPERT | OPT_VIDEO | OPT_AUDIO | OPT_DATA);
    if (show_advanced)
        show_help_options(options, "Advanced Subtitle options:",
                          OPT_EXPERT | OPT_SUBTITLE, OPT_VIDEO | OPT_AUDIO | OPT_DATA);

    if (show_advanced)
        show_help_options(options, "Data stream options:",
                          OPT_DATA, OPT_VIDEO | OPT_AUDIO | OPT_SUBTITLE);
    printf("\n");

    if (show_avoptions) {
        int flags = AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM;
        show_help_children(avcodec_get_class(), flags);
        show_help_children(avformat_get_class(), flags);
#if CONFIG_SWSCALE
        show_help_children(sws_get_class(), flags);
#endif
#if CONFIG_SWRESAMPLE
        show_help_children(swr_get_class(), AV_OPT_FLAG_AUDIO_PARAM);
#endif
        show_help_children(avfilter_get_class(), AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM);
        show_help_children(av_bsf_get_class(), AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_BSF_PARAM);
    }
}

void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Universal media converter\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] [[infile options] -i infile]... {[outfile options] outfile}...\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

enum OptGroup {
    GROUP_OUTFILE,
    GROUP_INFILE,
    GROUP_DECODER,
};

static const OptionGroupDef groups[] = {
    [GROUP_OUTFILE] = { "output url",  NULL, OPT_OUTPUT },
    [GROUP_INFILE]  = { "input url",   "i",  OPT_INPUT },
    [GROUP_DECODER] = { "loopback decoder", "dec", OPT_DECODER },
};

static int open_files(OptionGroupList *l, const char *inout, Scheduler *sch,
                      int (*open_file)(const OptionsContext*, const char*,
                                       Scheduler*))
{
    int i, ret;

    for (i = 0; i < l->nb_groups; i++) {
        OptionGroup *g = &l->groups[i];
        OptionsContext o;

        init_options(&o);
        o.g = g;

        ret = parse_optgroup(&o, g, options);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error parsing options for %s file "
                   "%s.\n", inout, g->arg);
            uninit_options(&o);
            return ret;
        }

        av_log(NULL, AV_LOG_DEBUG, "Opening an %s file: %s.\n", inout, g->arg);
        ret = open_file(&o, g->arg, sch);
        uninit_options(&o);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error opening %s file %s.\n",
                   inout, g->arg);
            return ret;
        }
        av_log(NULL, AV_LOG_DEBUG, "Successfully opened the file.\n");
    }

    return 0;
}

int ffmpeg_parse_options(int argc, char **argv, Scheduler *sch)
{
    GlobalOptionsContext go = { .sch = sch };
    OptionParseContext octx;
    const char *errmsg = NULL;
    int ret;

    memset(&octx, 0, sizeof(octx));

    /* split the commandline into an internal representation */
    ret = split_commandline(&octx, argc, argv, options, groups,
                            FF_ARRAY_ELEMS(groups));
    if (ret < 0) {
        errmsg = "splitting the argument list";
        goto fail;
    }

    /* apply global options */
    ret = parse_optgroup(&go, &octx.global_opts, options);
    if (ret < 0) {
        errmsg = "parsing global options";
        goto fail;
    }

    /* configure terminal and setup signal handlers */
    term_init();

    /* create complex filtergraphs */
    for (int i = 0; i < go.nb_filtergraphs; i++) {
        ret = fg_create(NULL, &go.filtergraphs[i], sch, NULL);
        go.filtergraphs[i] = NULL;
        if (ret < 0)
            goto fail;
    }

    /* open input files */
    ret = open_files(&octx.groups[GROUP_INFILE], "input", sch, ifile_open);
    if (ret < 0) {
        errmsg = "opening input files";
        goto fail;
    }

    /* open output files */
    ret = open_files(&octx.groups[GROUP_OUTFILE], "output", sch, of_open);
    if (ret < 0) {
        errmsg = "opening output files";
        goto fail;
    }

    /* create loopback decoders */
    ret = open_files(&octx.groups[GROUP_DECODER], "decoder", sch, dec_create);
    if (ret < 0) {
        errmsg = "creating loopback decoders";
        goto fail;
    }

    // bind unbound filtegraph inputs/outputs and check consistency
    ret = fg_finalise_bindings();
    if (ret < 0) {
        errmsg = "binding filtergraph inputs/outputs";
        goto fail;
    }

    correct_input_start_times();

    ret = apply_sync_offsets();
    if (ret < 0)
        goto fail;

fail:
    for (int i = 0; i < go.nb_filtergraphs; i++)
        av_freep(&go.filtergraphs[i]);
    av_freep(&go.filtergraphs);

    uninit_parse_context(&octx);
    if (ret < 0 && ret != AVERROR_EXIT) {
        av_log(NULL, AV_LOG_FATAL, "Error %s: %s\n",
               errmsg ? errmsg : "", av_err2str(ret));
    }
    return ret;
}

static int opt_progress(void *optctx, const char *opt, const char *arg)
{
    AVIOContext *avio = NULL;
    int ret;

    if (!strcmp(arg, "-"))
        arg = "pipe:";
    ret = avio_open2(&avio, arg, AVIO_FLAG_WRITE, &int_cb, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open progress URL \"%s\": %s\n",
               arg, av_err2str(ret));
        return ret;
    }
    progress_avio = avio;
    return 0;
}

int opt_timelimit(void *optctx, const char *opt, const char *arg)
{
#if HAVE_SETRLIMIT
    int ret;
    double lim;
    struct rlimit rl;

    ret = parse_number(opt, arg, OPT_TYPE_INT64, 0, INT_MAX, &lim);
    if (ret < 0)
        return ret;

    rl = (struct rlimit){ lim, lim + 1 };
    if (setrlimit(RLIMIT_CPU, &rl))
        perror("setrlimit");
#else
    av_log(NULL, AV_LOG_WARNING, "-%s not implemented on this OS\n", opt);
#endif
    return 0;
}

#if FFMPEG_OPT_QPHIST
static int opt_qphist(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "Option -%s is deprecated and has no effect\n", opt);
    return 0;
}
#endif

#if FFMPEG_OPT_ADRIFT_THRESHOLD
static int opt_adrift_threshold(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "Option -%s is deprecated and has no effect\n", opt);
    return 0;
}
#endif

static const char *const alt_channel_layout[] = { "ch_layout", NULL};
static const char *const alt_codec[]          = { "c", "acodec", "vcodec", "scodec", "dcodec", NULL };
static const char *const alt_filter[]         = { "af", "vf", NULL };
static const char *const alt_frames[]         = { "aframes", "vframes", "dframes", NULL };
static const char *const alt_pre[]            = { "apre", "vpre", "spre", NULL};
static const char *const alt_qscale[]         = { "q", NULL};
static const char *const alt_tag[]            = { "atag", "vtag", "stag", NULL };

#define OFFSET(x) offsetof(OptionsContext, x)
const OptionDef options[] = {
    /* main options */
    CMDUTILS_COMMON_OPTIONS
    { "f",                      OPT_TYPE_STRING, OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off       = OFFSET(format) },
        "force container format (auto-detected otherwise)", "fmt" },
    { "y",                      OPT_TYPE_BOOL, 0,
        {              &file_overwrite },
        "overwrite output files" },
    { "n",                      OPT_TYPE_BOOL, 0,
        {              &no_file_overwrite },
        "never overwrite output files" },
    { "ignore_unknown",         OPT_TYPE_BOOL, OPT_EXPERT,
        {              &ignore_unknown_streams },
        "Ignore unknown stream types" },
    { "copy_unknown",           OPT_TYPE_BOOL, OPT_EXPERT,
        {              &copy_unknown_streams },
        "Copy unknown stream types" },
    { "recast_media",           OPT_TYPE_BOOL, OPT_EXPERT,
        {              &recast_media },
        "allow recasting stream type in order to force a decoder of different media type" },
    { "c",                      OPT_TYPE_STRING, OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT | OPT_DECODER | OPT_HAS_CANON,
        { .off       = OFFSET(codec_names) },
        "select encoder/decoder ('copy' to copy stream without reencoding)", "codec",
        .u1.name_canon = "codec", },
    { "codec",                  OPT_TYPE_STRING, OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT | OPT_DECODER | OPT_EXPERT | OPT_HAS_ALT,
        { .off       = OFFSET(codec_names) },
        "alias for -c (select encoder/decoder)", "codec",
        .u1.names_alt = alt_codec, },
    { "pre",                    OPT_TYPE_STRING, OPT_PERSTREAM | OPT_OUTPUT | OPT_EXPERT | OPT_HAS_ALT,
        { .off       = OFFSET(presets) },
        "preset name", "preset",
        .u1.names_alt = alt_pre, },
    { "map",                    OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_map },
        "set input stream mapping",
        "[-]input_file_id[:stream_specifier][,sync_file_id[:stream_specifier]]" },
    { "map_metadata",           OPT_TYPE_STRING, OPT_SPEC | OPT_OUTPUT | OPT_EXPERT,
        { .off       = OFFSET(metadata_map) },
        "set metadata information of outfile from infile",
        "outfile[,metadata]:infile[,metadata]" },
    { "map_chapters",           OPT_TYPE_INT, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT,
        { .off = OFFSET(chapters_input_file) },
        "set chapters mapping", "input_file_index" },
    { "t",                      OPT_TYPE_TIME, OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(recording_time) },
        "stop transcoding after specified duration",
        "duration" },
    { "to",                     OPT_TYPE_TIME, OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(stop_time) },
        "stop transcoding after specified time is reached",
        "time_stop" },
    { "fs",                     OPT_TYPE_INT64, OPT_OFFSET | OPT_OUTPUT | OPT_EXPERT,
        { .off = OFFSET(limit_filesize) },
        "set the limit file size in bytes", "limit_size" },
    { "ss",                     OPT_TYPE_TIME, OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(start_time) },
        "start transcoding at specified time", "time_off" },
    { "sseof",                  OPT_TYPE_TIME, OPT_OFFSET | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(start_time_eof) },
        "set the start time offset relative to EOF", "time_off" },
    { "seek_timestamp",         OPT_TYPE_INT, OPT_OFFSET | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(seek_timestamp) },
        "enable/disable seeking by timestamp with -ss" },
    { "accurate_seek",          OPT_TYPE_BOOL, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(accurate_seek) },
        "enable/disable accurate seeking with -ss" },
    { "isync",                  OPT_TYPE_INT, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(input_sync_ref) },
        "Indicate the input index for sync reference", "sync ref" },
    { "itsoffset",              OPT_TYPE_TIME, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(input_ts_offset) },
        "set the input ts offset", "time_off" },
    { "itsscale",               OPT_TYPE_DOUBLE, OPT_PERSTREAM | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(ts_scale) },
        "set the input ts scale", "scale" },
    { "timestamp",              OPT_TYPE_FUNC,   OPT_FUNC_ARG | OPT_PERFILE | OPT_EXPERT | OPT_OUTPUT,
        { .func_arg = opt_recording_timestamp },
        "set the recording timestamp ('now' to set the current time)", "time" },
    { "metadata",               OPT_TYPE_STRING, OPT_SPEC | OPT_OUTPUT,
        { .off = OFFSET(metadata) },
        "add metadata", "key=value" },
    { "program",                OPT_TYPE_STRING, OPT_SPEC | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(program) },
        "add program with specified streams", "title=string:st=number..." },
    { "stream_group",           OPT_TYPE_STRING, OPT_SPEC | OPT_OUTPUT | OPT_EXPERT,
        { .off = OFFSET(stream_groups) },
        "add stream group with specified streams and group type-specific arguments", "id=number:st=number..." },
    { "dframes",                OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_PERFILE | OPT_EXPERT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_data_frames },
        "set the number of data frames to output", "number",
        .u1.name_canon = "frames" },
    { "benchmark",              OPT_TYPE_BOOL, OPT_EXPERT,
        { &do_benchmark },
        "add timings for benchmarking" },
    { "benchmark_all",          OPT_TYPE_BOOL, OPT_EXPERT,
        { &do_benchmark_all },
      "add timings for each task" },
    { "progress",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_progress },
      "write program-readable progress information", "url" },
    { "stdin",                  OPT_TYPE_BOOL, OPT_EXPERT,
        { &stdin_interaction },
      "enable or disable interaction on standard input" },
    { "timelimit",              OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_timelimit },
        "set max runtime in seconds in CPU user time", "limit" },
    { "dump",                   OPT_TYPE_BOOL, OPT_EXPERT,
        { &do_pkt_dump },
        "dump each input packet" },
    { "hex",                    OPT_TYPE_BOOL, OPT_EXPERT,
        { &do_hex_dump },
        "when dumping packets, also dump the payload" },
    { "re",                     OPT_TYPE_BOOL, OPT_EXPERT | OPT_OFFSET | OPT_INPUT,
        { .off = OFFSET(rate_emu) },
        "read input at native frame rate; equivalent to -readrate 1", "" },
    { "readrate",               OPT_TYPE_FLOAT, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(readrate) },
        "read input at specified rate", "speed" },
    { "readrate_initial_burst", OPT_TYPE_DOUBLE, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(readrate_initial_burst) },
        "The initial amount of input to burst read before imposing any readrate", "seconds" },
    { "readrate_catchup",       OPT_TYPE_FLOAT, OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(readrate_catchup) },
        "Temporary readrate used to catch up if an input lags behind the specified readrate", "speed" },
    { "target",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_PERFILE | OPT_EXPERT | OPT_OUTPUT,
        { .func_arg = opt_target },
        "specify target file type (\"vcd\", \"svcd\", \"dvd\", \"dv\" or \"dv50\" "
        "with optional prefixes \"pal-\", \"ntsc-\" or \"film-\")", "type" },
    { "frame_drop_threshold",   OPT_TYPE_FLOAT, OPT_EXPERT,
        { &frame_drop_threshold },
        "frame drop threshold", "" },
    { "copyts",                 OPT_TYPE_BOOL, OPT_EXPERT,
        { &copy_ts },
        "copy timestamps" },
    { "start_at_zero",          OPT_TYPE_BOOL, OPT_EXPERT,
        { &start_at_zero },
        "shift input timestamps to start at 0 when using copyts" },
    { "copytb",                 OPT_TYPE_INT, OPT_EXPERT,
        { &copy_tb },
        "copy input stream time base when stream copying", "mode" },
    { "shortest",               OPT_TYPE_BOOL, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT,
        { .off = OFFSET(shortest) },
        "finish encoding within shortest input" },
    { "shortest_buf_duration",  OPT_TYPE_FLOAT, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT,
        { .off = OFFSET(shortest_buf_duration) },
        "maximum buffering duration (in seconds) for the -shortest option" },
    { "bitexact",               OPT_TYPE_BOOL, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT | OPT_INPUT,
        { .off = OFFSET(bitexact) },
        "bitexact mode" },
    { "dts_delta_threshold",    OPT_TYPE_FLOAT, OPT_EXPERT,
        { &dts_delta_threshold },
        "timestamp discontinuity delta threshold", "threshold" },
    { "dts_error_threshold",    OPT_TYPE_FLOAT, OPT_EXPERT,
        { &dts_error_threshold },
        "timestamp error delta threshold", "threshold" },
    { "xerror",                 OPT_TYPE_BOOL, OPT_EXPERT,
        { &exit_on_error },
        "exit on error", "error" },
    { "abort_on",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_abort_on },
        "abort on the specified condition flags", "flags" },
    { "copyinkf",               OPT_TYPE_BOOL, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(copy_initial_nonkeyframes) },
        "copy initial non-keyframes" },
    { "copypriorss",            OPT_TYPE_INT, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(copy_prior_start) },
        "copy or discard frames before start time" },
    { "frames",                 OPT_TYPE_INT64, OPT_PERSTREAM | OPT_OUTPUT | OPT_EXPERT | OPT_HAS_ALT,
        { .off = OFFSET(max_frames) },
        "set the number of frames to output", "number",
        .u1.names_alt = alt_frames, },
    { "tag",                    OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT | OPT_INPUT | OPT_HAS_ALT,
        { .off = OFFSET(codec_tags) },
        "force codec tag/fourcc", "fourcc/tag",
        .u1.names_alt = alt_tag, },
    { "q",                      OPT_TYPE_DOUBLE, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT | OPT_HAS_CANON,
        { .off = OFFSET(qscale) },
        "use fixed quality scale (VBR)", "q",
        .u1.name_canon = "qscale", },
    { "qscale",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT | OPT_HAS_ALT,
        { .func_arg = opt_qscale },
        "use fixed quality scale (VBR)", "q",
        .u1.names_alt = alt_qscale, },
    { "profile",                OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_profile },
        "set profile", "profile" },
    { "filter",                 OPT_TYPE_STRING, OPT_PERSTREAM | OPT_OUTPUT | OPT_HAS_ALT,
        { .off = OFFSET(filters) },
        "apply specified filters to audio/video", "filter_graph",
        .u1.names_alt = alt_filter, },
    { "filter_threads",         OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_filter_threads },
        "number of non-complex filter threads" },
    { "filter_buffered_frames", OPT_TYPE_INT, OPT_EXPERT,
        { &filter_buffered_frames },
        "maximum number of buffered frames in a filter graph" },
#if FFMPEG_OPT_FILTER_SCRIPT
    { "filter_script",          OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(filter_scripts) },
        "deprecated, use -/filter", "filename" },
#endif
    { "reinit_filter",          OPT_TYPE_INT, OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(reinit_filters) },
        "reinit filtergraph on input parameter changes", "" },
    { "drop_changed",          OPT_TYPE_INT, OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(drop_changed) },
        "drop frame instead of reiniting filtergraph on input parameter changes", "" },
    { "filter_complex",         OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_filter_complex },
        "create a complex filtergraph", "graph_description" },
    { "filter_complex_threads", OPT_TYPE_INT, OPT_EXPERT,
        { &filter_complex_nbthreads },
        "number of threads for -filter_complex" },
    { "lavfi",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_filter_complex },
        "create a complex filtergraph", "graph_description" },
#if FFMPEG_OPT_FILTER_SCRIPT
    { "filter_complex_script", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_filter_complex_script },
        "deprecated, use -/filter_complex instead", "filename" },
#endif
    { "print_graphs",   OPT_TYPE_BOOL, 0,
        { &print_graphs },
        "print execution graph data to stderr" },
    { "print_graphs_file", OPT_TYPE_STRING, 0,
        { &print_graphs_file },
        "write execution graph data to the specified file", "filename" },
    { "print_graphs_format", OPT_TYPE_STRING, 0,
        { &print_graphs_format },
      "set the output printing format (available formats are: default, compact, csv, flat, ini, json, xml, mermaid, mermaidhtml)", "format" },
    { "auto_conversion_filters", OPT_TYPE_BOOL, OPT_EXPERT,
        { &auto_conversion_filters },
        "enable automatic conversion filters globally" },
    { "stats",               OPT_TYPE_BOOL, 0,
        { &print_stats },
        "print progress report during encoding", },
    { "stats_period",        OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_stats_period },
        "set the period at which ffmpeg updates stats and -progress output", "time" },
    { "attach",              OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_PERFILE | OPT_EXPERT | OPT_OUTPUT,
        { .func_arg = opt_attach },
        "add an attachment to the output file", "filename" },
    { "dump_attachment",     OPT_TYPE_STRING, OPT_SPEC | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(dump_attachment) },
        "extract an attachment into a file", "filename" },
    { "stream_loop",         OPT_TYPE_INT, OPT_EXPERT | OPT_INPUT | OPT_OFFSET,
        { .off = OFFSET(loop) }, "set number of times input stream shall be looped", "loop count" },
    { "debug_ts",            OPT_TYPE_BOOL, OPT_EXPERT,
        { &debug_ts },
        "print timestamp debugging info" },
    { "max_error_rate",      OPT_TYPE_FLOAT, OPT_EXPERT,
        { &max_error_rate },
        "ratio of decoding errors (0.0: no errors, 1.0: 100% errors) above which ffmpeg returns an error instead of success.", "maximum error rate" },
    { "discard",             OPT_TYPE_STRING, OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(discard) },
        "discard", "" },
    { "disposition",         OPT_TYPE_STRING, OPT_PERSTREAM | OPT_OUTPUT | OPT_EXPERT,
        { .off = OFFSET(disposition) },
        "disposition", "" },
    { "thread_queue_size",   OPT_TYPE_INT,  OPT_OFFSET | OPT_EXPERT | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(thread_queue_size) },
        "set the maximum number of queued packets from the demuxer" },
    { "find_stream_info",    OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT | OPT_OFFSET,
        { .off = OFFSET(find_stream_info) },
        "read and decode the streams to fill missing information with heuristics" },
    { "bits_per_raw_sample", OPT_TYPE_INT, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(bits_per_raw_sample) },
        "set the number of bits per raw sample", "number" },

    { "stats_enc_pre",      OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(enc_stats_pre)      },
        "write encoding stats before encoding" },
    { "stats_enc_post",     OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(enc_stats_post)     },
        "write encoding stats after encoding" },
    { "stats_mux_pre",      OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(mux_stats)          },
        "write packets stats before muxing" },
    { "stats_enc_pre_fmt",  OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(enc_stats_pre_fmt)  },
        "format of the stats written with -stats_enc_pre" },
    { "stats_enc_post_fmt", OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(enc_stats_post_fmt) },
        "format of the stats written with -stats_enc_post" },
    { "stats_mux_pre_fmt",  OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(mux_stats_fmt)      },
        "format of the stats written with -stats_mux_pre" },

    /* video options */
    { "vframes",                    OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT | OPT_EXPERT | OPT_HAS_CANON,
        { .func_arg = opt_video_frames },
        "set the number of video frames to output", "number",
        .u1.name_canon = "frames", },
    { "r",                          OPT_TYPE_STRING, OPT_VIDEO | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(frame_rates) },
        "override input framerate/convert to given output framerate (Hz value, fraction or abbreviation)", "rate" },
    { "fpsmax",                     OPT_TYPE_STRING, OPT_VIDEO | OPT_PERSTREAM | OPT_OUTPUT | OPT_EXPERT,
        { .off = OFFSET(max_frame_rates) },
        "set max frame rate (Hz value, fraction or abbreviation)", "rate" },
    { "s",                          OPT_TYPE_STRING, OPT_VIDEO | OPT_SUBTITLE | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(frame_sizes) },
        "set frame size (WxH or abbreviation)", "size" },
    { "aspect",                     OPT_TYPE_STRING, OPT_VIDEO | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(frame_aspect_ratios) },
        "set aspect ratio (4:3, 16:9 or 1.3333, 1.7777)", "aspect" },
    { "pix_fmt",                    OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(frame_pix_fmts) },
        "set pixel format", "format" },
    { "display_rotation",           OPT_TYPE_DOUBLE, OPT_VIDEO | OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(display_rotations) },
        "set pure counter-clockwise rotation in degrees for stream(s)",
        "angle" },
    { "display_hflip",              OPT_TYPE_BOOL,   OPT_VIDEO | OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(display_hflips) },
        "set display horizontal flip for stream(s) "
        "(overrides any display rotation if it is not set)"},
    { "display_vflip",              OPT_TYPE_BOOL,   OPT_VIDEO | OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(display_vflips) },
        "set display vertical flip for stream(s) "
        "(overrides any display rotation if it is not set)"},
    { "vn",                         OPT_TYPE_BOOL,   OPT_VIDEO | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(video_disable) },
        "disable video" },
    { "rc_override",                OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT  | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(rc_overrides) },
        "rate control override for specific intervals", "override" },
    { "vcodec",                     OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_PERFILE | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_video_codec },
        "alias for -c:v (select encoder/decoder for video streams)", "codec",
        .u1.name_canon = "codec", },
    { "timecode",                   OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT | OPT_EXPERT,
        { .func_arg = opt_timecode },
        "set initial TimeCode value.", "hh:mm:ss[:;.]ff" },
    { "pass",                       OPT_TYPE_INT,    OPT_VIDEO | OPT_PERSTREAM | OPT_OUTPUT | OPT_EXPERT,
        { .off = OFFSET(pass) },
        "select the pass number (1 to 3)", "n" },
    { "passlogfile",                OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(passlogfiles) },
        "select two pass log file name prefix", "prefix" },
    { "vstats",                     OPT_TYPE_FUNC,   OPT_VIDEO | OPT_EXPERT,
        { .func_arg = opt_vstats },
        "dump video coding statistics to file" },
    { "vstats_file",                OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_vstats_file },
        "dump video coding statistics to file", "file" },
    { "vstats_version",             OPT_TYPE_INT,    OPT_VIDEO | OPT_EXPERT,
        { &vstats_version },
        "Version of the vstats format to use."},
    { "vf",                         OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_video_filters },
        "alias for -filter:v (apply filters to video streams)", "filter_graph",
        .u1.name_canon = "filter", },
    { "intra_matrix",               OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(intra_matrices) },
        "specify intra matrix coeffs", "matrix" },
    { "inter_matrix",               OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(inter_matrices) },
        "specify inter matrix coeffs", "matrix" },
    { "chroma_intra_matrix",        OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(chroma_intra_matrices) },
        "specify intra matrix coeffs", "matrix" },
    { "vtag",                       OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_EXPERT  | OPT_PERFILE | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_old2new },
        "force video tag/fourcc", "fourcc/tag",
        .u1.name_canon = "tag", },
    { "fps_mode",                   OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(fps_mode) },
        "set framerate mode for matching video streams; overrides vsync" },
    { "force_fps",                  OPT_TYPE_BOOL,   OPT_VIDEO | OPT_EXPERT  | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(force_fps) },
        "force the selected framerate, disable the best supported framerate selection" },
    { "streamid",                   OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_streamid },
        "set the value of an outfile streamid", "streamIndex:value" },
    { "force_key_frames",           OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(forced_key_frames) },
        "force key frames at specified timestamps", "timestamps" },
    { "b",                          OPT_TYPE_FUNC,   OPT_VIDEO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_bitrate },
        "video bitrate (please use -b:v)", "bitrate" },
    { "hwaccel",                    OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT,
        { .off = OFFSET(hwaccels) },
        "use HW accelerated decoding", "hwaccel name" },
    { "hwaccel_device",             OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT,
        { .off = OFFSET(hwaccel_devices) },
        "select a device for HW acceleration", "devicename" },
    { "hwaccel_output_format",      OPT_TYPE_STRING, OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT,
        { .off = OFFSET(hwaccel_output_formats) },
        "select output format used with HW accelerated decoding", "format" },
    { "hwaccels",                   OPT_TYPE_FUNC,   OPT_EXIT | OPT_EXPERT,
        { .func_arg = show_hwaccels },
        "show available HW acceleration methods" },
    { "autorotate",                 OPT_TYPE_BOOL,   OPT_VIDEO | OPT_PERSTREAM | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(autorotate) },
        "automatically insert correct rotate filters" },
    { "autoscale",                  OPT_TYPE_BOOL,   OPT_VIDEO | OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(autoscale) },
        "automatically insert a scale filter at the end of the filter graph" },
    { "apply_cropping",             OPT_TYPE_STRING, OPT_VIDEO | OPT_PERSTREAM | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(apply_cropping) },
        "select the cropping to apply" },
    { "fix_sub_duration_heartbeat", OPT_TYPE_BOOL,   OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(fix_sub_duration_heartbeat) },
        "set this video output stream to be a heartbeat stream for "
        "fix_sub_duration, according to which subtitles should be split at "
        "random access points" },

    /* audio options */
    { "aframes",          OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT | OPT_EXPERT | OPT_HAS_CANON,
        { .func_arg = opt_audio_frames },
        "set the number of audio frames to output", "number",
        .u1.name_canon = "frames", },
    { "aq",               OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG  | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_audio_qscale },
        "set audio quality (codec-specific)", "quality", },
    { "ar",               OPT_TYPE_INT,     OPT_AUDIO | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(audio_sample_rate) },
        "set audio sampling rate (in Hz)", "rate" },
    { "ac",               OPT_TYPE_INT,     OPT_AUDIO | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(audio_channels) },
        "set number of audio channels", "channels" },
    { "an",               OPT_TYPE_BOOL,    OPT_AUDIO | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(audio_disable) },
        "disable audio" },
    { "acodec",           OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG  | OPT_PERFILE | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_audio_codec },
        "alias for -c:a (select encoder/decoder for audio streams)", "codec",
        .u1.name_canon = "codec", },
    { "ab",               OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG | OPT_PERFILE | OPT_OUTPUT,
        { .func_arg = opt_bitrate },
        "alias for -b:a (select bitrate for audio streams)", "bitrate" },
    { "apad",             OPT_TYPE_STRING,  OPT_AUDIO | OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(apad) },
        "audio pad", "" },
    { "atag",             OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG  | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_old2new },
        "force audio tag/fourcc", "fourcc/tag",
        .u1.name_canon = "tag", },
    { "sample_fmt",       OPT_TYPE_STRING,  OPT_AUDIO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(sample_fmts) },
        "set sample format", "format" },
    { "channel_layout",   OPT_TYPE_STRING,  OPT_AUDIO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT | OPT_HAS_ALT,
        { .off = OFFSET(audio_ch_layouts) },
        "set channel layout", "layout",
        .u1.names_alt = alt_channel_layout, },
    { "ch_layout",        OPT_TYPE_STRING,  OPT_AUDIO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .off = OFFSET(audio_ch_layouts) },
        "set channel layout", "layout",
        .u1.name_canon = "channel_layout", },
    { "af",               OPT_TYPE_FUNC,    OPT_AUDIO | OPT_FUNC_ARG  | OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_audio_filters },
        "alias for -filter:a (apply filters to audio streams)", "filter_graph",
        .u1.name_canon = "filter", },
    { "guess_layout_max", OPT_TYPE_INT,     OPT_AUDIO | OPT_PERSTREAM | OPT_EXPERT | OPT_INPUT,
        { .off = OFFSET(guess_layout_max) },
      "set the maximum number of channels to try to guess the channel layout" },

    /* subtitle options */
    { "sn",     OPT_TYPE_BOOL, OPT_SUBTITLE | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(subtitle_disable) },
        "disable subtitle" },
    { "scodec", OPT_TYPE_FUNC, OPT_SUBTITLE | OPT_FUNC_ARG  | OPT_PERFILE | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_subtitle_codec },
        "alias for -c:s (select encoder/decoder for subtitle streams)", "codec",
        .u1.name_canon = "codec", },
    { "stag",   OPT_TYPE_FUNC, OPT_SUBTITLE | OPT_FUNC_ARG  | OPT_EXPERT  | OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_old2new }
        , "force subtitle tag/fourcc", "fourcc/tag",
        .u1.name_canon = "tag" },
    { "fix_sub_duration", OPT_TYPE_BOOL, OPT_EXPERT | OPT_SUBTITLE | OPT_PERSTREAM | OPT_INPUT,
        { .off = OFFSET(fix_sub_duration) },
        "fix subtitles duration" },
    { "canvas_size", OPT_TYPE_STRING, OPT_SUBTITLE | OPT_PERSTREAM | OPT_INPUT | OPT_EXPERT,
        { .off = OFFSET(canvas_sizes) },
        "set canvas size (WxH or abbreviation)", "size" },

    /* muxer options */
    { "muxdelay",   OPT_TYPE_FLOAT, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT,
        { .off = OFFSET(mux_max_delay) },
        "set the maximum demux-decode delay", "seconds" },
    { "muxpreload", OPT_TYPE_FLOAT, OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT,
        { .off = OFFSET(mux_preload) },
        "set the initial demux-decode delay", "seconds" },
    { "sdp_file",   OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT | OPT_OUTPUT,
        { .func_arg = opt_sdp_file },
        "specify a file in which to print sdp information", "file" },

    { "time_base",     OPT_TYPE_STRING, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(time_bases) },
        "set the desired time base hint for output stream (1:24, 1:48000 or 0.04166, 2.0833e-5)", "ratio" },
    { "enc_time_base", OPT_TYPE_STRING, OPT_EXPERT | OPT_PERSTREAM | OPT_OUTPUT,
        { .off = OFFSET(enc_time_bases) },
        "set the desired time base for the encoder (1:24, 1:48000 or 0.04166, 2.0833e-5). "
        "two special values are defined - "
        "0 = use frame rate (video) or sample rate (audio),"
        "-1 = match source time base", "ratio" },

    { "bsf", OPT_TYPE_STRING, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT | OPT_INPUT,
        { .off = OFFSET(bitstream_filters) },
        "A comma-separated list of bitstream filters", "bitstream_filters", },

    { "apre", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_AUDIO | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_preset },
        "set the audio options to the indicated preset", "preset",
        .u1.name_canon = "pre", },
    { "vpre", OPT_TYPE_FUNC, OPT_VIDEO | OPT_FUNC_ARG | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_preset },
        "set the video options to the indicated preset", "preset",
        .u1.name_canon = "pre", },
    { "spre", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_SUBTITLE | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_preset },
        "set the subtitle options to the indicated preset", "preset",
        .u1.name_canon = "pre", },
    { "fpre", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_preset },
        "set options from indicated preset file", "filename",
        .u1.name_canon = "pre", },

    { "max_muxing_queue_size", OPT_TYPE_INT, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(max_muxing_queue_size) },
        "maximum number of packets that can be buffered while waiting for all streams to initialize", "packets" },
    { "muxing_queue_data_threshold", OPT_TYPE_INT, OPT_PERSTREAM | OPT_EXPERT | OPT_OUTPUT,
        { .off = OFFSET(muxing_queue_data_threshold) },
        "set the threshold after which max_muxing_queue_size is taken into account", "bytes" },

    /* data codec support */
    { "dcodec", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_DATA | OPT_PERFILE | OPT_EXPERT | OPT_INPUT | OPT_OUTPUT | OPT_HAS_CANON,
        { .func_arg = opt_data_codec },
        "alias for -c:d (select encoder/decoder for data streams)", "codec",
        .u1.name_canon = "codec", },
    { "dn", OPT_TYPE_BOOL, OPT_DATA | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(data_disable) }, "disable data" },

#if CONFIG_VAAPI
    { "vaapi_device", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_vaapi_device },
        "set VAAPI hardware device (DirectX adapter index, DRM path or X11 display name)", "device" },
#endif

#if CONFIG_QSV
    { "qsv_device", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_qsv_device },
        "set QSV hardware device (DirectX adapter index, DRM path or X11 display name)", "device"},
#endif

    { "init_hw_device", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_init_hw_device },
        "initialise hardware device", "args" },
    { "filter_hw_device", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_filter_hw_device },
        "set hardware device used when filtering", "device" },

    // deprecated options
#if FFMPEG_OPT_ADRIFT_THRESHOLD
    { "adrift_threshold", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_adrift_threshold },
        "deprecated, does nothing", "threshold" },
#endif
#if FFMPEG_OPT_TOP
    { "top", OPT_TYPE_INT,     OPT_VIDEO | OPT_EXPERT | OPT_PERSTREAM | OPT_INPUT | OPT_OUTPUT,
        { .off = OFFSET(top_field_first) },
        "deprecated, use the setfield video filter", "" },
#endif
#if FFMPEG_OPT_QPHIST
    { "qphist", OPT_TYPE_FUNC, OPT_VIDEO | OPT_EXPERT,
        { .func_arg = opt_qphist },
        "deprecated, does nothing" },
#endif
#if FFMPEG_OPT_VSYNC
    { "vsync",                  OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT,
        { .func_arg = opt_vsync },
        "set video sync method globally; deprecated, use -fps_mode", "" },
#endif

    { NULL, },
};


/* ========== fftools/ffmpeg.c ========== */

/*
 * Copyright (c) 2000-2003 Fabrice Bellard
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
 * multimedia converter based on the FFmpeg libraries
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif
#if HAVE_SETCONSOLECTRLHANDLER
#include <windows.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif

#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"

#include "libavdevice/avdevice.h"

#include "cmdutils.h"
#if CONFIG_MEDIACODEC
#include "compat/android/binder.h"
#endif
#include "ffmpeg.h"
#include "ffmpeg_sched.h"
#include "ffmpeg_utils.h"
#include "graphprint.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

FILE *vstats_file;

typedef struct BenchmarkTimeStamps {
    int64_t real_usec;
    int64_t user_usec;
    int64_t sys_usec;
} BenchmarkTimeStamps;

static BenchmarkTimeStamps get_benchmark_time_stamps(void);
static int64_t getmaxrss(void);

atomic_uint nb_output_dumped = 0;

static BenchmarkTimeStamps current_time;
AVIOContext *progress_avio = NULL;

InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

Decoder     **decoders;
int        nb_decoders;

#if HAVE_TERMIOS_H

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int restore_tty;
#endif

static void term_exit_sigsafe(void)
{
#if HAVE_TERMIOS_H
    if(restore_tty)
        tcsetattr (0, TCSANOW, &oldtty);
#endif
}

void term_exit(void)
{
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    term_exit_sigsafe();
}

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static atomic_int transcode_init_done = 0;
static volatile int ffmpeg_exited = 0;
static int64_t copy_ts_first_pts = AV_NOPTS_VALUE;

static void
sigterm_handler(int sig)
{
    int ret;
    received_sigterm = sig;
    received_nb_signals++;
    term_exit_sigsafe();
    if(received_nb_signals > 3) {
        ret = write(2/*STDERR_FILENO*/, "Received > 3 system signals, hard exiting\n",
                    strlen("Received > 3 system signals, hard exiting\n"));
        if (ret < 0) { /* Do nothing */ };
        exit(123);
    }
}

#if HAVE_SETCONSOLECTRLHANDLER
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    av_log(NULL, AV_LOG_DEBUG, "\nReceived windows signal %ld\n", fdwCtrlType);

    switch (fdwCtrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        sigterm_handler(SIGINT);
        return TRUE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sigterm_handler(SIGTERM);
        /* Basically, with these 3 events, when we return from this method the
           process is hard terminated, so stall as long as we need to
           to try and let the main thread(s) clean up and gracefully terminate
           (we have at most 5 seconds, but should be done far before that). */
        while (!ffmpeg_exited) {
            Sleep(0);
        }
        return TRUE;

    default:
        av_log(NULL, AV_LOG_ERROR, "Received unknown windows signal %ld\n", fdwCtrlType);
        return FALSE;
    }
}
#endif

#ifdef __linux__
#define SIGNAL(sig, func)               \
    do {                                \
        action.sa_handler = func;       \
        sigaction(sig, &action, NULL);  \
    } while (0)
#else
#define SIGNAL(sig, func) \
    signal(sig, func)
#endif

void term_init(void)
{
#if defined __linux__
    struct sigaction action = {0};
    action.sa_handler = sigterm_handler;

    /* block other interrupts while processing this one */
    sigfillset(&action.sa_mask);

    /* restart interruptible functions (i.e. don't fail with EINTR)  */
    action.sa_flags = SA_RESTART;
#endif

#if HAVE_TERMIOS_H
    if (stdin_interaction) {
        struct termios tty;
        if (tcgetattr (0, &tty) == 0) {
            oldtty = tty;
            restore_tty = 1;

            tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                             |INLCR|IGNCR|ICRNL|IXON);
            tty.c_oflag |= OPOST;
            tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
            tty.c_cflag &= ~(CSIZE|PARENB);
            tty.c_cflag |= CS8;
            tty.c_cc[VMIN] = 1;
            tty.c_cc[VTIME] = 0;

            tcsetattr (0, TCSANOW, &tty);
        }
        SIGNAL(SIGQUIT, sigterm_handler); /* Quit (POSIX).  */
    }
#endif

    SIGNAL(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    SIGNAL(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
#ifdef SIGXCPU
    SIGNAL(SIGXCPU, sigterm_handler);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* Broken pipe (POSIX). */
#endif
#if HAVE_SETCONSOLECTRLHANDLER
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);
#endif
}

/* read a key without blocking */
static int read_key(void)
{
#if HAVE_TERMIOS_H
    int n = 1;
    struct timeval tv;
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(1, &rfds, NULL, NULL, &tv);
    if (n > 0) {
        unsigned char ch;
        n = read(0, &ch, 1);
        if (n == 1)
            return ch;

        return n;
    }
#elif HAVE_KBHIT
#    if HAVE_PEEKNAMEDPIPE && HAVE_GETSTDHANDLE
    static int is_pipe;
    static HANDLE input_handle;
    DWORD dw, nchars;
    if(!input_handle){
        input_handle = GetStdHandle(STD_INPUT_HANDLE);
        is_pipe = !GetConsoleMode(input_handle, &dw);
    }

    if (is_pipe) {
        /* When running under a GUI, you will end here. */
        if (!PeekNamedPipe(input_handle, NULL, 0, NULL, &nchars, NULL)) {
            // input pipe may have been closed by the program that ran ffmpeg
            return -1;
        }
        //Read it
        if(nchars != 0) {
            unsigned char ch;
            if (read(0, &ch, 1) == 1)
                return ch;
            return 0;
        }else{
            return -1;
        }
    }
#    endif
    if(kbhit())
        return(getch());
#endif
    return -1;
}

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > atomic_load(&transcode_init_done);
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void ffmpeg_cleanup(int ret)
{
    if ((print_graphs || print_graphs_file) && nb_output_files > 0)
        print_filtergraphs(filtergraphs, nb_filtergraphs, input_files, nb_input_files, output_files, nb_output_files);

    if (do_benchmark) {
        int64_t maxrss = getmaxrss() / 1024;
        av_log(NULL, AV_LOG_INFO, "bench: maxrss=%"PRId64"KiB\n", maxrss);
    }

    for (int i = 0; i < nb_filtergraphs; i++)
        fg_free(&filtergraphs[i]);
    av_freep(&filtergraphs);

    for (int i = 0; i < nb_output_files; i++)
        of_free(&output_files[i]);

    for (int i = 0; i < nb_input_files; i++)
        ifile_close(&input_files[i]);

    for (int i = 0; i < nb_decoders; i++)
        dec_free(&decoders[i]);
    av_freep(&decoders);

    if (vstats_file) {
        if (fclose(vstats_file))
            av_log(NULL, AV_LOG_ERROR,
                   "Error closing vstats file, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
    }
    av_freep(&vstats_filename);
    of_enc_stats_close();

    hw_device_free_all();

    av_freep(&filter_nbthreads);

    av_freep(&print_graphs_file);
    av_freep(&print_graphs_format);

    av_freep(&input_files);
    av_freep(&output_files);

    uninit_opts();

    avformat_network_deinit();

    if (received_sigterm) {
        av_log(NULL, AV_LOG_INFO, "Exiting normally, received signal %d.\n",
               (int) received_sigterm);
    } else if (ret && atomic_load(&transcode_init_done)) {
        av_log(NULL, AV_LOG_INFO, "Conversion failed!\n");
    }
    term_exit();
    ffmpeg_exited = 1;
}

OutputStream *ost_iter(OutputStream *prev)
{
    int of_idx  = prev ? prev->file->index : 0;
    int ost_idx = prev ? prev->index + 1  : 0;

    for (; of_idx < nb_output_files; of_idx++) {
        OutputFile *of = output_files[of_idx];
        if (ost_idx < of->nb_streams)
            return of->streams[ost_idx];

        ost_idx = 0;
    }

    return NULL;
}

InputStream *ist_iter(InputStream *prev)
{
    int if_idx  = prev ? prev->file->index : 0;
    int ist_idx = prev ? prev->index + 1  : 0;

    for (; if_idx < nb_input_files; if_idx++) {
        InputFile *f = input_files[if_idx];
        if (ist_idx < f->nb_streams)
            return f->streams[ist_idx];

        ist_idx = 0;
    }

    return NULL;
}

static void frame_data_free(void *opaque, uint8_t *data)
{
    FrameData *fd = (FrameData *)data;

    av_frame_side_data_free(&fd->side_data, &fd->nb_side_data);
    avcodec_parameters_free(&fd->par_enc);

    av_free(data);
}

static int frame_data_ensure(AVBufferRef **dst, int writable)
{
    AVBufferRef *src = *dst;

    if (!src || (writable && !av_buffer_is_writable(src))) {
        FrameData *fd;

        fd = av_mallocz(sizeof(*fd));
        if (!fd)
            return AVERROR(ENOMEM);

        *dst = av_buffer_create((uint8_t *)fd, sizeof(*fd),
                                frame_data_free, NULL, 0);
        if (!*dst) {
            av_buffer_unref(&src);
            av_freep(&fd);
            return AVERROR(ENOMEM);
        }

        if (src) {
            const FrameData *fd_src = (const FrameData *)src->data;

            memcpy(fd, fd_src, sizeof(*fd));
            fd->par_enc = NULL;
            fd->side_data = NULL;
            fd->nb_side_data = 0;

            if (fd_src->par_enc) {
                int ret = 0;

                fd->par_enc = avcodec_parameters_alloc();
                ret = fd->par_enc ?
                      avcodec_parameters_copy(fd->par_enc, fd_src->par_enc) :
                      AVERROR(ENOMEM);
                if (ret < 0) {
                    av_buffer_unref(dst);
                    av_buffer_unref(&src);
                    return ret;
                }
            }

            if (fd_src->nb_side_data) {
                int ret = clone_side_data(&fd->side_data, &fd->nb_side_data,
                                          fd_src->side_data, fd_src->nb_side_data, 0);
                if (ret < 0) {
                    av_buffer_unref(dst);
                    av_buffer_unref(&src);
                    return ret;
                }
            }

            av_buffer_unref(&src);
        } else {
            fd->dec.frame_num = UINT64_MAX;
            fd->dec.pts       = AV_NOPTS_VALUE;

            for (unsigned i = 0; i < FF_ARRAY_ELEMS(fd->wallclock); i++)
                fd->wallclock[i] = INT64_MIN;
        }
    }

    return 0;
}

FrameData *frame_data(AVFrame *frame)
{
    int ret = frame_data_ensure(&frame->opaque_ref, 1);
    return ret < 0 ? NULL : (FrameData*)frame->opaque_ref->data;
}

const FrameData *frame_data_c(AVFrame *frame)
{
    int ret = frame_data_ensure(&frame->opaque_ref, 0);
    return ret < 0 ? NULL : (const FrameData*)frame->opaque_ref->data;
}

FrameData *packet_data(AVPacket *pkt)
{
    int ret = frame_data_ensure(&pkt->opaque_ref, 1);
    return ret < 0 ? NULL : (FrameData*)pkt->opaque_ref->data;
}

const FrameData *packet_data_c(AVPacket *pkt)
{
    int ret = frame_data_ensure(&pkt->opaque_ref, 0);
    return ret < 0 ? NULL : (const FrameData*)pkt->opaque_ref->data;
}

int check_avoptions_used(const AVDictionary *opts, const AVDictionary *opts_used,
                         void *logctx, int decode)
{
    const AVClass  *class = avcodec_get_class();
    const AVClass *fclass = avformat_get_class();

    const int flag = decode ? AV_OPT_FLAG_DECODING_PARAM :
                              AV_OPT_FLAG_ENCODING_PARAM;
    const AVDictionaryEntry *e = NULL;

    while ((e = av_dict_iterate(opts, e))) {
        const AVOption *option, *foption;
        char *optname, *p;

        if (av_dict_get(opts_used, e->key, NULL, 0))
            continue;

        optname = av_strdup(e->key);
        if (!optname)
            return AVERROR(ENOMEM);

        p = strchr(optname, ':');
        if (p)
            *p = 0;

        option = av_opt_find(&class, optname, NULL, 0,
                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        foption = av_opt_find(&fclass, optname, NULL, 0,
                              AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        av_freep(&optname);
        if (!option || foption)
            continue;

        if (!(option->flags & flag)) {
            av_log(logctx, AV_LOG_ERROR, "Codec AVOption %s (%s) is not a %s "
                   "option.\n", e->key, option->help ? option->help : "",
                   decode ? "decoding" : "encoding");
            return AVERROR(EINVAL);
        }

        av_log(logctx, AV_LOG_WARNING, "Codec AVOption %s (%s) has not been used "
               "for any stream. The most likely reason is either wrong type "
               "(e.g. a video option with no video streams) or that it is a "
               "private option of some decoder which was not actually used "
               "for any stream.\n", e->key, option->help ? option->help : "");
    }

    return 0;
}

void update_benchmark(const char *fmt, ...)
{
    if (do_benchmark_all) {
        BenchmarkTimeStamps t = get_benchmark_time_stamps();
        va_list va;
        char buf[1024];

        if (fmt) {
            va_start(va, fmt);
            vsnprintf(buf, sizeof(buf), fmt, va);
            va_end(va);
            av_log(NULL, AV_LOG_INFO,
                   "bench: %8" PRIu64 " user %8" PRIu64 " sys %8" PRIu64 " real %s \n",
                   t.user_usec - current_time.user_usec,
                   t.sys_usec - current_time.sys_usec,
                   t.real_usec - current_time.real_usec, buf);
        }
        current_time = t;
    }
}

static void print_report(int is_last_report, int64_t timer_start, int64_t cur_time, int64_t pts)
{
    AVBPrint buf, buf_script;
    int64_t total_size = of_filesize(output_files[0]);
    int vid;
    double bitrate;
    double speed;
    static int64_t last_time = -1;
    static int first_report = 1;
    uint64_t nb_frames_dup = 0, nb_frames_drop = 0;
    int mins, secs, ms, us;
    int64_t hours;
    const char *hours_sign;
    int ret;
    float t;

    if (!print_stats && !is_last_report && !progress_avio)
        return;

    if (!is_last_report) {
        if (last_time == -1) {
            last_time = cur_time;
        }
        if (((cur_time - last_time) < stats_period && !first_report) ||
            (first_report && atomic_load(&nb_output_dumped) < nb_output_files))
            return;
        last_time = cur_time;
    }

    t = (cur_time-timer_start) / 1000000.0;

    vid = 0;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprint_init(&buf_script, 0, AV_BPRINT_SIZE_AUTOMATIC);

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        const float q = ost->enc ? atomic_load(&ost->quality) / (float) FF_QP2LAMBDA : -1;

        if (vid && ost->type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&buf, "q=%2.1f ", q);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file->index, ost->index, q);
        }
        if (!vid && ost->type == AVMEDIA_TYPE_VIDEO) {
            float fps;
            uint64_t frame_number = atomic_load(&ost->packets_written);

            fps = t > 1 ? frame_number / t : 0;
            av_bprintf(&buf, "frame=%5"PRId64" fps=%3.*f q=%3.1f ",
                     frame_number, fps < 9.95, fps, q);
            av_bprintf(&buf_script, "frame=%"PRId64"\n", frame_number);
            av_bprintf(&buf_script, "fps=%.2f\n", fps);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file->index, ost->index, q);
            if (is_last_report)
                av_bprintf(&buf, "L");

            if (ost->filter) {
                nb_frames_dup  = atomic_load(&ost->filter->nb_frames_dup);
                nb_frames_drop = atomic_load(&ost->filter->nb_frames_drop);
            }

            vid = 1;
        }
    }

    if (copy_ts) {
        if (copy_ts_first_pts == AV_NOPTS_VALUE && pts > 1)
            copy_ts_first_pts = pts;
        if (copy_ts_first_pts != AV_NOPTS_VALUE)
            pts -= copy_ts_first_pts;
    }

    us    = FFABS64U(pts) % AV_TIME_BASE;
    secs  = FFABS64U(pts) / AV_TIME_BASE % 60;
    mins  = FFABS64U(pts) / AV_TIME_BASE / 60 % 60;
    hours = FFABS64U(pts) / AV_TIME_BASE / 3600;
    hours_sign = (pts < 0) ? "-" : "";

    bitrate = pts != AV_NOPTS_VALUE && pts && total_size >= 0 ? total_size * 8 / (pts / 1000.0) : -1;
    speed   = pts != AV_NOPTS_VALUE && t != 0.0 ? (double)pts / AV_TIME_BASE / t : -1;

    if (total_size < 0) av_bprintf(&buf, "size=N/A time=");
    else                av_bprintf(&buf, "size=%8.0fKiB time=", total_size / 1024.0);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf, "N/A ");
    } else {
        av_bprintf(&buf, "%s%02"PRId64":%02d:%02d.%02d ",
                   hours_sign, hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    if (bitrate < 0) {
        av_bprintf(&buf, "bitrate=N/A");
        av_bprintf(&buf_script, "bitrate=N/A\n");
    }else{
        av_bprintf(&buf, "bitrate=%6.1fkbits/s", bitrate);
        av_bprintf(&buf_script, "bitrate=%6.1fkbits/s\n", bitrate);
    }

    if (total_size < 0) av_bprintf(&buf_script, "total_size=N/A\n");
    else                av_bprintf(&buf_script, "total_size=%"PRId64"\n", total_size);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf_script, "out_time_us=N/A\n");
        av_bprintf(&buf_script, "out_time_ms=N/A\n");
        av_bprintf(&buf_script, "out_time=N/A\n");
    } else {
        av_bprintf(&buf_script, "out_time_us=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time_ms=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time=%s%02"PRId64":%02d:%02d.%06d\n",
                   hours_sign, hours, mins, secs, us);
    }

    if (nb_frames_dup || nb_frames_drop)
        av_bprintf(&buf, " dup=%"PRId64" drop=%"PRId64, nb_frames_dup, nb_frames_drop);
    av_bprintf(&buf_script, "dup_frames=%"PRId64"\n", nb_frames_dup);
    av_bprintf(&buf_script, "drop_frames=%"PRId64"\n", nb_frames_drop);

    if (speed < 0) {
        av_bprintf(&buf, " speed=N/A");
        av_bprintf(&buf_script, "speed=N/A\n");
    } else {
        av_bprintf(&buf, " speed=%4.3gx", speed);
        av_bprintf(&buf_script, "speed=%4.3gx\n", speed);
    }

    secs = (int)t;
    ms = (int)((t - secs) * 1000);
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;

    av_bprintf(&buf, " elapsed=%"PRId64":%02d:%02d.%02d", hours, mins, secs, ms / 10);

    if (print_stats || is_last_report) {
        const char end = is_last_report ? '\n' : '\r';
        if (print_stats==1 && AV_LOG_INFO > av_log_get_level()) {
            fprintf(stderr, "%s    %c", buf.str, end);
        } else
            av_log(NULL, AV_LOG_INFO, "%s    %c", buf.str, end);

        fflush(stderr);
    }
    av_bprint_finalize(&buf, NULL);

    if (progress_avio) {
        av_bprintf(&buf_script, "progress=%s\n",
                   is_last_report ? "end" : "continue");
        avio_write(progress_avio, buf_script.str,
                   FFMIN(buf_script.len, buf_script.size - 1));
        avio_flush(progress_avio);
        av_bprint_finalize(&buf_script, NULL);
        if (is_last_report) {
            if ((ret = avio_closep(&progress_avio)) < 0)
                av_log(NULL, AV_LOG_ERROR,
                       "Error closing progress log, loss of information possible: %s\n", av_err2str(ret));
        }
    }

    first_report = 0;
}

static void print_stream_maps(void)
{
    av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");
    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        for (int j = 0; j < ist->nb_filters; j++) {
            if (!filtergraph_is_simple(ist->filters[j]->graph)) {
                av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
                       ist->file->index, ist->index, ist->dec ? ist->dec->name : "?",
                       ist->filters[j]->name);
                if (nb_filtergraphs > 1)
                    av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
                av_log(NULL, AV_LOG_INFO, "\n");
            }
        }
    }

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        if (ost->attachment_filename) {
            /* an attached file */
            av_log(NULL, AV_LOG_INFO, "  File %s -> Stream #%d:%d\n",
                   ost->attachment_filename, ost->file->index, ost->index);
            continue;
        }

        if (ost->filter && !filtergraph_is_simple(ost->filter->graph)) {
            /* output from a complex graph */
            av_log(NULL, AV_LOG_INFO, "  %s", ost->filter->name);
            if (nb_filtergraphs > 1)
                av_log(NULL, AV_LOG_INFO, " (graph %d)", ost->filter->graph->index);

            av_log(NULL, AV_LOG_INFO, " -> Stream #%d:%d (%s)\n", ost->file->index,
                   ost->index, ost->enc->enc_ctx->codec->name);
            continue;
        }

        av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d -> #%d:%d",
               ost->ist->file->index,
               ost->ist->index,
               ost->file->index,
               ost->index);
        if (ost->enc) {
            const AVCodec *in_codec    = ost->ist->dec;
            const AVCodec *out_codec   = ost->enc->enc_ctx->codec;
            const char *decoder_name   = "?";
            const char *in_codec_name  = "?";
            const char *encoder_name   = "?";
            const char *out_codec_name = "?";
            const AVCodecDescriptor *desc;

            if (in_codec) {
                decoder_name  = in_codec->name;
                desc = avcodec_descriptor_get(in_codec->id);
                if (desc)
                    in_codec_name = desc->name;
                if (!strcmp(decoder_name, in_codec_name))
                    decoder_name = "native";
            }

            if (out_codec) {
                encoder_name   = out_codec->name;
                desc = avcodec_descriptor_get(out_codec->id);
                if (desc)
                    out_codec_name = desc->name;
                if (!strcmp(encoder_name, out_codec_name))
                    encoder_name = "native";
            }

            av_log(NULL, AV_LOG_INFO, " (%s (%s) -> %s (%s))",
                   in_codec_name, decoder_name,
                   out_codec_name, encoder_name);
        } else
            av_log(NULL, AV_LOG_INFO, " (copy)");
        av_log(NULL, AV_LOG_INFO, "\n");
    }
}

static void set_tty_echo(int on)
{
#if HAVE_TERMIOS_H
    struct termios tty;
    if (tcgetattr(0, &tty) == 0) {
        if (on) tty.c_lflag |= ECHO;
        else    tty.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &tty);
    }
#endif
}

static int check_keyboard_interaction(int64_t cur_time)
{
    int i, key;
    static int64_t last_time;
    /* read_key() returns 0 on EOF */
    if (cur_time - last_time >= 100000) {
        key =  read_key();
        last_time = cur_time;
    }else
        key = -1;
    if (key == 'q') {
        av_log(NULL, AV_LOG_INFO, "\n\n[q] command received. Exiting.\n\n");
        return AVERROR_EXIT;
    }
    if (key == '+') av_log_set_level(av_log_get_level()+10);
    if (key == '-') av_log_set_level(av_log_get_level()-10);
    if (key == 'c' || key == 'C'){
        char buf[4096], target[64], command[256], arg[256] = {0};
        double time;
        int k, n = 0;
        fprintf(stderr, "\nEnter command: <target>|all <time>|-1 <command>[ <argument>]\n");
        i = 0;
        set_tty_echo(1);
        while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
            if (k > 0)
                buf[i++] = k;
        buf[i] = 0;
        set_tty_echo(0);
        fprintf(stderr, "\n");
        if (k > 0 &&
            (n = sscanf(buf, "%63[^ ] %lf %255[^ ] %255[^\n]", target, &time, command, arg)) >= 3) {
            av_log(NULL, AV_LOG_DEBUG, "Processing command target:%s time:%f command:%s arg:%s",
                   target, time, command, arg);
            for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
                if (ost->fg_simple)
                    fg_send_command(ost->fg_simple, time, target, command, arg,
                                    key == 'C');
            }
            for (i = 0; i < nb_filtergraphs; i++)
                fg_send_command(filtergraphs[i], time, target, command, arg,
                                key == 'C');
        } else {
            av_log(NULL, AV_LOG_ERROR,
                   "Parse error, at least 3 arguments were expected, "
                   "only %d given in string '%s'\n", n, buf);
        }
    }
    if (key == '?'){
        fprintf(stderr, "key    function\n"
                        "?      show this help\n"
                        "+      increase verbosity\n"
                        "-      decrease verbosity\n"
                        "c      Send command to first matching filter supporting it\n"
                        "C      Send/Queue command to all matching filters\n"
                        "h      dump packets/hex press to cycle through the 3 states\n"
                        "q      quit\n"
                        "s      Show QP histogram\n"
        );
    }
    return 0;
}

/*
 * The following code is the main loop of the file converter
 */
static int transcode(Scheduler *sch)
{
    int ret = 0;
    int64_t timer_start, transcode_ts = 0;

    print_stream_maps();

    atomic_store(&transcode_init_done, 1);

    ret = sch_start(sch);
    if (ret < 0)
        return ret;

    if (stdin_interaction) {
        av_log(NULL, AV_LOG_INFO, "Press [q] to stop, [?] for help\n");
    }

    timer_start = av_gettime_relative();

    while (!sch_wait(sch, stats_period, &transcode_ts)) {
        int64_t cur_time= av_gettime_relative();

        if (received_nb_signals)
            break;

        /* if 'q' pressed, exits */
        if (stdin_interaction)
            if (check_keyboard_interaction(cur_time) < 0)
                break;

        /* dump report by using the output first video and audio streams */
        print_report(0, timer_start, cur_time, transcode_ts);
    }

    ret = sch_stop(sch, &transcode_ts);

    /* write the trailer if needed */
    for (int i = 0; i < nb_output_files; i++) {
        int err = of_write_trailer(output_files[i]);
        ret = err_merge(ret, err);
    }

    term_exit();

    /* dump report by using the first video and audio streams */
    print_report(1, timer_start, av_gettime_relative(), transcode_ts);

    return ret;
}

static BenchmarkTimeStamps get_benchmark_time_stamps(void)
{
    BenchmarkTimeStamps time_stamps = { av_gettime_relative() };
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    time_stamps.user_usec =
        (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
    time_stamps.sys_usec =
        (rusage.ru_stime.tv_sec * 1000000LL) + rusage.ru_stime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    proc = GetCurrentProcess();
    GetProcessTimes(proc, &c, &e, &k, &u);
    time_stamps.user_usec =
        ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    time_stamps.sys_usec =
        ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
#else
    time_stamps.user_usec = time_stamps.sys_usec = 0;
#endif
    return time_stamps;
}

static int64_t getmaxrss(void)
{
#if HAVE_GETRUSAGE && HAVE_STRUCT_RUSAGE_RU_MAXRSS
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    return (int64_t)rusage.ru_maxrss * 1024;
#elif HAVE_GETPROCESSMEMORYINFO
    HANDLE proc;
    PROCESS_MEMORY_COUNTERS memcounters;
    proc = GetCurrentProcess();
    memcounters.cb = sizeof(memcounters);
    GetProcessMemoryInfo(proc, &memcounters, sizeof(memcounters));
    return memcounters.PeakPagefileUsage;
#else
    return 0;
#endif
}

int main(int argc, char **argv)
{
    Scheduler *sch = NULL;

    int ret;
    BenchmarkTimeStamps ti;

    init_dynload();

    setvbuf(stderr,NULL,_IONBF,0); /* win32 runtime needs this */

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    show_banner(argc, argv, options);

    sch = sch_alloc();
    if (!sch) {
        ret = AVERROR(ENOMEM);
        goto finish;
    }

    /* parse options and open all input/output files */
    ret = ffmpeg_parse_options(argc, argv, sch);
    if (ret < 0)
        goto finish;

    if (nb_output_files <= 0 && nb_input_files == 0) {
        show_usage();
        av_log(NULL, AV_LOG_WARNING, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        ret = 1;
        goto finish;
    }

    if (nb_output_files <= 0) {
        av_log(NULL, AV_LOG_FATAL, "At least one output file must be specified\n");
        ret = 1;
        goto finish;
    }

#if CONFIG_MEDIACODEC
    android_binder_threadpool_init_if_required();
#endif

    current_time = ti = get_benchmark_time_stamps();
    ret = transcode(sch);
    if (ret >= 0 && do_benchmark) {
        int64_t utime, stime, rtime;
        current_time = get_benchmark_time_stamps();
        utime = current_time.user_usec - ti.user_usec;
        stime = current_time.sys_usec  - ti.sys_usec;
        rtime = current_time.real_usec - ti.real_usec;
        av_log(NULL, AV_LOG_INFO,
               "bench: utime=%0.3fs stime=%0.3fs rtime=%0.3fs\n",
               utime / 1000000.0, stime / 1000000.0, rtime / 1000000.0);
    }

    ret = received_nb_signals                 ? 255 :
          (ret == FFMPEG_ERROR_RATE_EXCEEDED) ?  69 : ret;

finish:
    if (ret == AVERROR_EXIT)
        ret = 0;

    ffmpeg_cleanup(ret);

    sch_free(&sch);

    av_log(NULL, AV_LOG_VERBOSE, "\n");
    av_log(NULL, AV_LOG_VERBOSE, "Exiting with exit code %d\n", ret);

    return ret;
}

