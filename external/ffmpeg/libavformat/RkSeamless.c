/*
 * RockChip Seemless demuxer
 * Copyright (c) 2013 Rockchip
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
 * RockChip Seemless demuxer
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "libavutil/time.h"
#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"
#include "url.h"
#include <pthread.h>


#define SEAMLESS_INITIAL_BUFFER_SIZE 32768
#define SEAMLESS_DEBUG 0

#if SEAMLESS_DEBUG
#define SEAMLESS_LOG(...)   av_log(NULL, AV_LOG_INFO, __VA_ARGS__)
#else
#define SEAMLESS_LOG(...)   do { if (0) av_log(NULL, AV_LOG_INFO, __VA_ARGS__); } while (0)
#endif

#define RK_SEAMLESS_IDENTIFIER_TAG "#HISIPLAY"
#define RK_SEAMLESS_START "#HISIPLAY_START_SEAMLESS"
#define RK_SEAMLESS_STREAM "#HISIPLAY_STREAM:"

#define RK_SEAM_SEG_UPDATE_THRD 5
#define RK_SEAM_FLV_READ_FAIL_TOLERAT_THRD 5
#define RK_SEAM_MAX_STREAMS 16
#define RK_SEAM_DROP_PKT_CNT 8

/*
 * An apple http stream consists of a playlist with media segment files,
 * played sequentially. There may be several playlists with the same
 * video content, in different bandwidth variants, that are played in
 * parallel (preferably only one bandwidth variant at a time). In this case,
 * the user supplied the url to a main playlist that only lists the variant
 * playlists.
 *
 * If the main playlist doesn't point at any variants, we still create
 * one anonymous toplevel variant for this, to maintain the structure.
 */

enum SeamKeyType {
    SEAM_KEY_NONE,
    SEAM_KEY_AES_128,
};

struct SeamSegment {
    double duration;
    int discontinuity;
    char url[MAX_URL_SIZE];
    char key[MAX_URL_SIZE];
    enum SeamKeyType key_type;
    uint8_t iv[16];
};

struct streamTimeUpdateContext {
    int64_t segBaseTimeStrmScale;       /* with time scale of stream */
    int64_t segThreshold;               /* with time scale of stream */
    int64_t prePktPts;
    int64_t prePktDts;
    int32_t haveJumpCnt;
    int32_t forceUpdateInSeek;
};

typedef struct forceDropAudio {
    int drop_cnt;
}forceDropAudio;

typedef enum {
    HAVE_SEEK_FLAG                  = 0x01,
    HAVE_OPEN_URL                   = 0x02,
    HAVE_DONE_SUB_FORMAT_SEEK       = 0x04,
}RKSEAMLESS_SEEK_MAP;

/*
 * Each variant has its own demuxer. If it currently is active,
 * it has an open AVIOContext too, and potentially an AVPacket
 * containing the next packet from this stream.
 */
struct SeamVariant {
    int bandwidth;
    char url[MAX_URL_SIZE];
    AVIOContext pb;
    uint8_t* read_buffer;
    URLContext *input;
    AVFormatContext *parent;
    int index;
    AVFormatContext *ctx;
    AVPacket pkt;
    int stream_offset;

    int finished;
    int target_duration;
    int start_seq_no;
    int n_segments;
    struct SeamSegment **segments;
    int needed, cur_needed;
    int cur_seq_no;
    int64_t last_load_time;
    int64_t segBaseTimeUs;
    struct streamTimeUpdateContext upCtx[RK_SEAM_MAX_STREAMS];
    forceDropAudio drop_audio[RK_SEAM_MAX_STREAMS];
    char key_url[MAX_URL_SIZE];
    uint8_t key[16];

    int readPktErrCnt;
    int newUrlAfterSeek;
    pthread_mutex_t mutex;
};

typedef struct SeamContext {
    int n_variants;
    struct SeamVariant **variants;
    int cur_seq_no;
    int end_of_segment;
    int first_packet;
    int64_t first_timestamp;
    int64_t seek_timestamp;
    int64_t seek_origin_timestamp;
    int seek_flags;
    int abort;
    AVIOInterruptCB *interrupt_callback;
} SeamContext;

static int read_chomp_line(AVIOContext *s, char *buf, int maxlen)
{
    int len = ff_get_line(s, buf, maxlen);
    while (len > 0 && isspace(buf[len - 1]))
        buf[--len] = '\0';
    return len;
}

static void free_segment_list(struct SeamVariant *var)
{
    int i;
    for (i = 0; i < var->n_segments; i++)
        av_free(var->segments[i]);
    av_freep(&var->segments);
    var->n_segments = 0;
}

static void free_variant_list(SeamContext *c)
{
    int i;
    for (i = 0; i < c->n_variants; i++) {
        struct SeamVariant *var = c->variants[i];

        free_segment_list(var);
        av_free_packet(&var->pkt);
        av_free(var->pb.buffer);

        if (var->input)
            ffurl_close(var->input);

        if (var->ctx) {
            avformat_close_input(&var->ctx);
        }

        av_free(var);
    }
    av_freep(&c->variants);
    c->n_variants = 0;
}

/*
 * Used to reset a statically allocated AVPacket to a clean slate,
 * containing no data.
 */
static void reset_packet(AVPacket *pkt)
{
    av_init_packet(pkt);
    pkt->data = NULL;
}

static void flush_avio_buffer(struct SeamVariant *var)
{
    if (var ==NULL)
        return;

    /* flush avio buffer in flv format */
    if (!strcmp(var->ctx->iformat->name, "flv")) {
        int len = var->ctx->pb->buf_end - var->ctx->pb->buf_ptr;
        char buf[300] = {0};
        int canRead = len >=300 ? 300 : len;
        while (len) {
            canRead = len >=300 ? 300 : len;
            avio_read(var->ctx->pb, buf, canRead);
            len -=canRead;
        }
    }
}

static void reset_demuxer_if_necessary(struct SeamVariant *var)
{
    int i =0;

    if (var == NULL) {
        return;
    }

    if (var->ctx && var->ctx->pb) {
        if (var->ctx->iformat &&
            (!strcmp(var->ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") ||
             !strcmp(var->ctx->iformat->name, "flv"))) {
            if (var->ctx->iformat->read_close) {
                var->ctx->iformat->read_close(var->ctx);
            }

            for(i=var->ctx->nb_streams-1; i>=0; i--) {
                ff_free_stream(var->ctx, var->ctx->streams[i]);
            }

            var->ctx->pb->pos =0;
            var->ctx->nb_streams = 0;
			var->readPktErrCnt =0;
        }
    }

    return;
}

static int flv_read_seamless_seek(AVFormatContext *s, int stream_index,
    int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    if (s == NULL) {
        return -1;
    }
    int index = -1;
    int64_t ret =0, seekPos = 0, curPos =0;
    AVStream *st = NULL;
    AVIndexEntry *ie = NULL;
    int avioBufLen =0;

    if(stream_index <0)
        return -1;

    SEAMLESS_LOG("flv_subformat_read_seek in, s: 0x%X, cur pos: %lld\n",
        s, avio_tell(s->pb));

    st = s->streams[stream_index];

    SEAMLESS_LOG("stream_index: %d, st: %p, s->nb_streams: %d, st->nb_index_entries: %d\n",
        stream_index, st, s->nb_streams, st->nb_index_entries);

    if (!st || (stream_index >=s->nb_streams)) {
        return -1;
    }

    if (st->nb_index_entries >=1) {
        SEAMLESS_LOG("ts: %lld, idx[0]->ts: %lld, pos: %lld, idx[nb_index-1]->ts: %lld, pos: %lld\n",
            ts, st->index_entries[0].timestamp, st->index_entries[0].pos,
            st->index_entries[st->nb_index_entries-1].timestamp,
            st->index_entries[st->nb_index_entries-1].pos);
    }

    if (st->index_entries) {
        if (ts <=st->index_entries[0].timestamp) {
            seekPos = st->index_entries[0].pos;
            goto FLV_SEEK_SEAMLESS_OUT;
        } else if (ts >=st->index_entries[st->nb_index_entries-1].timestamp) {
            seekPos = st->index_entries[st->nb_index_entries-1].pos;
            goto FLV_SEEK_SEAMLESS_OUT;
        }
    }
    for (int k=0; k <st->nb_index_entries; k++) {
        SEAMLESS_LOG("index[%d].timestamp: %lld, pos: %lld\n",
            k, st->index_entries[k].timestamp, st->index_entries[k].pos);
    }

    index = av_index_search_timestamp(st, ts, flags);
    SEAMLESS_LOG("after search, index: %d, nb_index_entries: %d\n",
        index, st->nb_index_entries);

    if((index < 0) || (index >=st->nb_index_entries))
        return -1;

    seekPos = st->index_entries[index].pos;
FLV_SEEK_SEAMLESS_OUT:
    if (seekPos >0) {
        SEAMLESS_LOG("seek to index: %d, seekPos: %lld, seekTime: %lld\n",
            index, seekPos, st->index_entries[index].timestamp);
        avio_seek(s->pb, seekPos, SEEK_SET);
    }

    SEAMLESS_LOG("flv_read_seamless_seek out, cur pos: %lld, seekPos: %lld\n",
        avio_tell(s->pb), seekPos);

    return 0;
}

static struct SeamVariant *new_variant(SeamContext *c, int bandwidth,
                                   const char *url, const char *base)
{
    struct SeamVariant *var = av_mallocz(sizeof(struct SeamVariant));
    if (!var)
        return NULL;

    reset_packet(&var->pkt);
    var->bandwidth = bandwidth;
    ff_make_absolute_url(var->url, sizeof(var->url), base, url);
    dynarray_add(&c->variants, &c->n_variants, var);
    return var;
}

struct seam_variant_info {
    char bandwidth[20];
};

static void handle_variant_args(struct seam_variant_info *info, const char *key,
                                int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "BANDWIDTH=", key_len)) {
        *dest     =        info->bandwidth;
        *dest_len = sizeof(info->bandwidth);
    }
}

struct seam_key_info {
     char uri[MAX_URL_SIZE];
     char method[10];
     char iv[35];
};

static void handle_key_args(struct seam_key_info *info, const char *key,
                            int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "METHOD=", key_len)) {
        *dest     =        info->method;
        *dest_len = sizeof(info->method);
    } else if (!strncmp(key, "URI=", key_len)) {
        *dest     =        info->uri;
        *dest_len = sizeof(info->uri);
    } else if (!strncmp(key, "IV=", key_len)) {
        *dest     =        info->iv;
        *dest_len = sizeof(info->iv);
    }
}

static int parse_playlist(SeamContext *c, const char *url,
                          struct SeamVariant *var, AVIOContext *in)
{
    SEAMLESS_LOG("Seamless parse_playlist in\n");

    int ret = 0,is_segment = 0,is_discontinuity = 0, is_variant = 0, bandwidth = 0;
    double duration = 0;
    enum SeamKeyType key_type = SEAM_KEY_NONE;
    uint8_t iv[16] = "";
    int has_iv = 0;
    char key[MAX_URL_SIZE] = "";
    char line[1024];
    const char *ptr;
    int close_in = 0;
    if (!in) {
        AVDictionary *opts = NULL;
        close_in = 1;
        /* Some Seamless servers dont like being sent the range header */
        av_dict_set(&opts, "seekable", "0", 0);
        av_dict_set(&opts, "user-agent","Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 Mobile/7B334b Safari/531.21.10",0);
        ret = avio_open2(&in, url, AVIO_FLAG_READ,
                         c->interrupt_callback, &opts);

        av_dict_free(&opts);
        if (ret < 0)
            return ret;
    }

    read_chomp_line(in, line, sizeof(line));
    if (strcmp(line, RK_SEAMLESS_IDENTIFIER_TAG)) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    read_chomp_line(in, line, sizeof(line));
    if (strcmp(line, RK_SEAMLESS_START)) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    if (var) {
        free_segment_list(var);
        var->finished = 0;
    }
    while (!url_feof(in)) {
        read_chomp_line(in, line, sizeof(line));
        if (av_strstart(line, "#EXT-X-STREAM-INF:", &ptr)) {
            struct seam_variant_info info = {{0}};
            is_variant = 1;
            SEAMLESS_LOG("parse_playlist, EXT-X-STREAM-INF\n");

            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_variant_args,
                               &info);
            bandwidth = atoi(info.bandwidth);
        } else if (av_strstart(line, "#EXT-X-KEY:", &ptr)) {
            struct seam_key_info info = {{0}};
            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_key_args,
                               &info);
            key_type = SEAM_KEY_NONE;
            has_iv = 0;
            if (!strcmp(info.method, "AES-128"))
                key_type = SEAM_KEY_AES_128;
            if (!strncmp(info.iv, "0x", 2) || !strncmp(info.iv, "0X", 2)) {
                ff_hex_to_data(iv, info.iv + 2);
                has_iv = 1;
            }
            av_strlcpy(key, info.uri, sizeof(key));
        } else if (av_strstart(line, "#EXT-X-TARGETDURATION:", &ptr)) {
            if (!var) {
                var = new_variant(c, 0, in->url, NULL);
                if (!var) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            }
            var->target_duration = atoi(ptr);
        } else if (av_strstart(line, "#EXT-X-MEDIA-SEQUENCE:", &ptr)) {
            if (!var) {
                var = new_variant(c, 0, in->url, NULL);
                if (!var) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            }
            var->start_seq_no = atoi(ptr);
        } else if (av_strstart(line, "#HISIPLAY_ENDLIST", &ptr)) {
            if (var)
                var->finished = 1;
        } else if (av_strstart(line, "#HISIPLAY_STREAM:", &ptr)) {
            is_segment = 1;
            duration   = strtod(ptr,NULL);
        }else if(av_strstart(line, "#EXT-X-DISCONTINUITY",NULL)){
           is_discontinuity = 1;
        } else if (av_strstart(line, "#", NULL)) {
            continue;
        } else if (line[0]) {
            if (is_variant) {
                SEAMLESS_LOG("parse_playlist, 1st url = %s\n", in->url);

                if (!new_variant(c, bandwidth, line, in->url)) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                is_variant = 0;
                bandwidth  = 0;
            }
            if (is_segment) {
                struct SeamSegment *seg;
                if (!var) {
                    var = new_variant(c, 0, in->url, NULL);
                    if (!var) {
                        ret = AVERROR(ENOMEM);
                        goto fail;
                    }
                }

                seg = av_malloc(sizeof(struct SeamSegment));
                if (!seg) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                seg->duration = duration;
                seg->key_type = key_type;

                if(is_discontinuity){
                    seg->discontinuity = 1;
                    is_discontinuity = 0;
                }else{
                    seg->discontinuity = 0;
                }
                if (has_iv) {
                    memcpy(seg->iv, iv, sizeof(iv));
                } else {
                    int seq = var->start_seq_no + var->n_segments;
                    memset(seg->iv, 0, sizeof(seg->iv));
                    AV_WB32(seg->iv + 12, seq);
                }

                SEAMLESS_LOG("queue : %d segment, durMs: %f, url: %s\n",
                    var->n_segments, seg->duration, line);

                ff_make_absolute_url(seg->key, sizeof(seg->key), in->url, key);
                ff_make_absolute_url(seg->url, sizeof(seg->url), in->url, line);
                dynarray_add(&var->segments, &var->n_segments, seg);
                is_segment = 0;
            }
        }
    }
    if (var)
        var->last_load_time = av_gettime();

fail:
    if (close_in)
        avio_close(in);
    return ret;
}

static int open_input(struct SeamVariant *var)
{
    AVDictionary *opts = NULL;
    int ret, i =0;

    struct SeamSegment *seg = var->segments[var->cur_seq_no - var->start_seq_no];
    av_dict_set(&opts, "seekable", "1", 0);
    av_dict_set(&opts, "timeout", "500000", 0);
    av_dict_set(&opts, "user-agent","Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 Mobile/7B334b Safari/531.21.10",0);

    /* now open a new url of SeamSegment */
    if (var->cur_seq_no >=0) {
        SEAMLESS_LOG("open_input, cur_seq: %d, seg->key_type: %d, seg->url: %s\n",
            var->cur_seq_no, seg->key_type, seg->url);
    }

    if (var->newUrlAfterSeek & HAVE_SEEK_FLAG) {
        var->newUrlAfterSeek |=HAVE_OPEN_URL;
        var->newUrlAfterSeek &=(~HAVE_SEEK_FLAG);
    }

    var->segBaseTimeUs =0;
    if (var->cur_seq_no >var->start_seq_no) {
        for (i =0; i <RK_SEAM_MAX_STREAMS; i++) {
            var->drop_audio[i].drop_cnt =RK_SEAM_DROP_PKT_CNT;
        }
    }

    /* calculate the base time of segments */
    for (i =0; i <var->cur_seq_no; i++) {
        if (var->segments[i]) {
            var->segBaseTimeUs +=(var->segments[i]->duration*1000000);
        }
    }
    SEAMLESS_LOG("open_input, segBaseTimeUs is: %lld\n", var->segBaseTimeUs);

    if (seg->key_type == SEAM_KEY_NONE) {
        ret = ffurl_open(&var->input, seg->url, AVIO_FLAG_READ,
                          &var->parent->interrupt_callback, &opts);
        if (var->cur_seq_no >=0) {
            SEAMLESS_LOG("open_input, ret: %d, s->pos: %lld\n",
                ret, var->pb.pos);
            if (var->ctx && var->ctx->pb) {
                SEAMLESS_LOG("var->ctx->pb pos: %lld\n", var->ctx->pb->pos);
            }
            if (!ret) {
                reset_demuxer_if_necessary(var);
            }
        }
        goto cleanup;
    } else if (seg->key_type == SEAM_KEY_AES_128) {
        char iv[33], key[33], url[MAX_URL_SIZE];
        if (strcmp(seg->key, var->key_url)) {
            URLContext *uc;
            if (ffurl_open(&uc, seg->key, AVIO_FLAG_READ,
                           &var->parent->interrupt_callback, &opts) == 0) {
                if (ffurl_read_complete(uc, var->key, sizeof(var->key))
                    != sizeof(var->key)) {
                    av_log(NULL, AV_LOG_ERROR, "Unable to read key file %s\n",
                           seg->key);
                }
                ffurl_close(uc);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Unable to open key file %s\n",
                       seg->key);
            }
            av_strlcpy(var->key_url, seg->key, sizeof(var->key_url));
        }
        ff_data_to_hex(iv, seg->iv, sizeof(seg->iv), 0);
        ff_data_to_hex(key, var->key, sizeof(var->key), 0);
        iv[32] = key[32] = '\0';
        if (strstr(seg->url, "://"))
            snprintf(url, sizeof(url), "crypto+%s", seg->url);
        else
            snprintf(url, sizeof(url), "crypto:%s", seg->url);
        if ((ret = ffurl_alloc(&var->input, url, AVIO_FLAG_READ,
                               &var->parent->interrupt_callback)) < 0)
            goto cleanup;
        av_opt_set(var->input->priv_data, "key", key, 0);
        av_opt_set(var->input->priv_data, "iv", iv, 0);
        /* Need to repopulate options */
        av_dict_free(&opts);
        av_dict_set(&opts, "seekable", "0", 0);
        av_dict_set(&opts, "user-agent","Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 Mobile/7B334b Safari/531.21.10",0);
        av_dict_set(&opts, "timeout", "500000", 0);
        if ((ret = ffurl_connect(var->input, &opts)) < 0) {
            ffurl_close(var->input);
            var->input = NULL;
            goto cleanup;
        }
        ret = 0;
    }
    else
      ret = AVERROR(ENOSYS);

cleanup:
    av_dict_free(&opts);
    return ret;
}

static int64_t read_seek(void *opaque, int64_t offset, int whence)
{
    struct SeamVariant *v = opaque;
    if ((v == NULL) || (v->input == NULL)) {
        SEAMLESS_LOG("read_seek error, input parameter invalid\n");
        return -1;
    }

    int avioBufSize = v->ctx->pb->buf_end - v->ctx->pb->buf_ptr;
    /*SEAMLESS_LOG("read_seek in, offset: %lld, avio_buf size: %d, newUrlAfterSeek: 0x%X\n",
        offset, avioBufSize, v->newUrlAfterSeek);*/

    int64_t ret = 0;
    int seekTryCnt = 5;
    while(seekTryCnt--) {
        ret = ffurl_seek(v->input, offset, whence);
        if (ret <0) {
            av_usleep(500000);
            SEAMLESS_LOG("read_seek, seek offset: %lld fail, try time: %d th\n",
                offset, seekTryCnt);
        } else {
            break;
        }
    }
    //SEAMLESS_LOG("read_seek ret: %lld\n", ret);
    return ret;
}

static int read_data(void *opaque, uint8_t *buf, int buf_size)
{
    struct SeamVariant *v = opaque;
    SeamContext *c = v->parent->priv_data;
    int ret, i,retry = 0,parse_list_retry = 20,read_timeout_cnt = 0;
    int64_t last_load_timeUs = av_gettime();
    if(v->parent->exit_flag){
        return AVERROR_EOF;
    }

    int haveOpenInput =0;

restart:
    if (!v->input) {
        /* If this is a live stream and the reload interval has elapsed since
         * the last playlist reload, reload the variant playlists now. */
        int64_t reload_interval = v->n_segments > 0 ?
                                  v->segments[v->n_segments - 1]->duration :
                                  v->target_duration;
        reload_interval *= 1000000;

reload:
        if (c->cur_seq_no >=1) {
            SEAMLESS_LOG("cur_seq: %d, finished: %d\n",
                c->cur_seq_no, v->finished);
        }

        if (!v->finished &&
            av_gettime() - v->last_load_time >= reload_interval) {
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0){
                parse_list_retry--;
                if(parse_list_retry < 0){
                    av_log(NULL, AV_LOG_ERROR,"parse_playlist return ret = %d",ret);
                    return ret;
                }
                av_usleep(1000*1000);
            }
            /* If we need to reload the playlist again below (if
             * there's still no more segments), switch to a reload
             * interval of half the target duration. */
            reload_interval = v->target_duration * 500000LL;
        }
        if (v->cur_seq_no < v->start_seq_no) {
            av_log(NULL, AV_LOG_WARNING,
                   "skipping %d segments ahead, expired from playlists\n",
                   v->start_seq_no - v->cur_seq_no);
            v->cur_seq_no = v->start_seq_no;
        }

        if (c->cur_seq_no >=1) {
            SEAMLESS_LOG("cur_seq: %d, start_seq_no: %d, n_segments: %d\n",
                c->cur_seq_no, v->start_seq_no, v->n_segments);
        }

        if (v->cur_seq_no >= v->start_seq_no + v->n_segments) {
            if (v->finished){
                av_log(NULL, AV_LOG_ERROR, "finished Seamless read data AVERROR_EOF\n");
                return AVERROR_EOF;
            }
            while (av_gettime() - v->last_load_time < reload_interval) {
                if (ff_check_interrupt(c->interrupt_callback))
                    return AVERROR_EXIT;
                av_usleep(100*1000);
            }
            /* Enough time has elapsed since the last reload */
            goto reload;
        }

        if (c->cur_seq_no >=1) {
            SEAMLESS_LOG("cur_seq: %d, call open_input\n", c->cur_seq_no);
        }

        ret = open_input(v);
        if (ret < 0){
            if((av_gettime() - last_load_timeUs) >= 60000000){
                ffurl_close(v->input);
                v->input = NULL;
                v->cur_seq_no++;
                av_log(NULL, AV_LOG_ERROR, "Seamless read data skip current url");
                c->end_of_segment = 1;
                c->cur_seq_no = v->cur_seq_no;
                last_load_timeUs = av_gettime();

                return ret;
            }else{
               if(c->abort || v->parent->exit_flag){
                     return AVERROR_EOF;
                }
                av_log(NULL, AV_LOG_ERROR,"open_input reload");
                av_usleep(200*1000);
                goto reload;
            }
        }else {
            haveOpenInput = 1;
        }
    }
    last_load_timeUs = av_gettime();
    retry = 15;
    int notGetDataCnt =0;
    while(retry--){
        ret = ffurl_read(v->input, buf, buf_size);
        if (haveOpenInput) {
            SEAMLESS_LOG("ffurl_read read %d bytes after open one new input, ret: %d\n",
                buf_size, ret);
        }
        if (ret > 0){
            return ret;
        }
        if(ret == 0){
            notGetDataCnt++;
            SEAMLESS_LOG("%d th not get any data from net....\n", notGetDataCnt);
            if (notGetDataCnt >=5) {
                break;
            }
        } else if (ret == AVERROR(ETIMEDOUT)) {
            retry++;
            SEAMLESS_LOG("ffurl_read timeout in read_data of RkSeamless\n");
        } else {
            SEAMLESS_LOG("ffurl_read <0 in read_data of RkSeamless, ret: %d\n", ret);
        }

        if(c->abort || v->parent->exit_flag){
            av_log(NULL, AV_LOG_ERROR,"ffurl_read ret = %d",ret);
            return AVERROR_EOF;
        }
    }

    SEAMLESS_LOG("now we will open next url of the playList\n");
    ffurl_close(v->input);
    v->input = NULL;
    v->cur_seq_no++;

    c->end_of_segment = 1;
    c->cur_seq_no = v->cur_seq_no;
    read_timeout_cnt = 0;

    if (v->ctx && v->ctx->nb_streams && v->parent->nb_streams >= v->stream_offset + v->ctx->nb_streams) {
        v->needed = 0;
        for (i = v->stream_offset; i < v->stream_offset + v->ctx->nb_streams;
             i++) {
            if (v->parent->streams[i]->discard < AVDISCARD_ALL)
                v->needed = 1;
        }
    }

    SEAMLESS_LOG("read_data, v->needed: %d, cur_seq_no: %d, start_seq_no: %d\n",
        v->needed, c->cur_seq_no, v->start_seq_no);

    if (!v->needed) {
        av_log(v->parent, AV_LOG_ERROR, "No longer receiving variant %d\n",
               v->index);
        return AVERROR_EOF;
    }
    goto restart;
}

static int seamless_read_header(AVFormatContext *s)
{
    SEAMLESS_LOG("seamless_read_header in\n");

    SeamContext *c = s->priv_data;
    int ret = 0, i, j, stream_offset = 0,retry = 0;
    c->interrupt_callback = &s->interrupt_callback;

loadplaylist:
    if ((ret = parse_playlist(c, s->filename, NULL, s->pb)) < 0){
        if(retry > 5){
        goto fail;
        }else{
            if(ret == AVERROR_EXIT || s->exit_flag){
                ret = AVERROR_EOF;
                goto fail;
            }
            retry++;
            av_usleep(100*1000);
            goto loadplaylist;
        }
    }

    if (c->n_variants == 0) {
        av_log(NULL, AV_LOG_WARNING, "Empty playlist\n");
        ret = AVERROR_EOF;
        goto fail;
    }
    /* If the playlist only contained variants, parse each individual
     * variant playlist. */
    retry = 0;
loadplaylist1:
    if (c->n_variants > 1 || c->variants[0]->n_segments == 0) {
       // for (i = 0; i < c->n_variants; i++) {
            struct SeamVariant *v = c->variants[0];
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0){
                if(retry > 5){
                goto fail;
                }else{
                    if(ret == AVERROR_EXIT || s->exit_flag){
                        ret = AVERROR_EOF;
                        goto fail;
                    }
                    retry++;
                    av_usleep(100*1000);
                    goto loadplaylist1;
        }
            }
    }

    if (c->variants[0]->n_segments == 0) {
        av_log(NULL, AV_LOG_WARNING, "Empty playlist\n");
        ret = AVERROR_EOF;
        goto fail;
    }

    SEAMLESS_LOG("after parse list, live stream flag: %d\n",
        !c->variants[0]->finished);

    /* If this isn't a live stream, calculate the total duration of the
     * stream. */
    if (c->variants[0]->finished) {
        double duration = 0;
        for (i = 0; i < c->variants[0]->n_segments; i++)
            duration += c->variants[0]->segments[i]->duration;
        s->duration = duration * AV_TIME_BASE;
    }

    /* Open the demuxer for each variant */
//    for (i = 0; i < c->n_variants; i++) {
        i = 0;
        struct SeamVariant *v = c->variants[i];
        AVInputFormat *in_fmt = NULL;
        char bitrate_str[20];
   //     if (v->n_segments == 0)
         //   continue;

        if (!(v->ctx = avformat_alloc_context())) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        v->index  = i;
        v->needed = 1;
        v->parent = s;
        memset(&v->upCtx, 0, sizeof(v->upCtx));
        memset(&v->drop_audio, 0, sizeof(v->drop_audio));

        pthread_mutex_init(&v->mutex, NULL);

        /* If this is a live stream with more than 3 segments, start at the
         * third last segment. */
        v->cur_seq_no = v->start_seq_no;
        if (!v->finished && v->n_segments > 3)
            v->cur_seq_no = v->start_seq_no + v->n_segments - 3;

        v->read_buffer = av_malloc(SEAMLESS_INITIAL_BUFFER_SIZE);

        ffio_init_context(&v->pb, v->read_buffer, SEAMLESS_INITIAL_BUFFER_SIZE, 0, v,
                          read_data, NULL, read_seek);

        v->pb.seekable = 0;
        ret = av_probe_input_buffer(&v->pb, &in_fmt, v->segments[0]->url,
                                    NULL, 0, 0);
        if (ret < 0) {
            /* Free the ctx - it isn't initialized properly at this point,
             * so avformat_close_input shouldn't be called. If
             * avformat_open_input fails below, it frees and zeros the
             * context, so it doesn't need any special treatment like this. */
            av_log(s, AV_LOG_ERROR, "Error when loading first segment '%s'\n", v->segments[0]->url);
            avformat_free_context(v->ctx);
            v->ctx = NULL;
            goto fail;
        }

        if (in_fmt && in_fmt->name && !strcmp(in_fmt->name, "flv")) {
            v->pb.seekable = 1;
        } else {
            v->pb.seekable = 0;
            v->pb.seamless_mp4 = 1;
        }
        if (v->ctx->pb) {
            av_free(v->ctx->pb);
            v->ctx->pb = NULL;
        }

        if (!v->ctx->pb) {
            v->ctx->pb = (AVIOContext*)av_malloc(sizeof(AVIOContext));
            if (!v->ctx->pb) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
        memcpy(v->ctx->pb, &v->pb, sizeof(AVIOContext));

        SEAMLESS_LOG("read_header, will call avformat_open_input, v->pb: %p\n", v->ctx->pb);

        ret = avformat_open_input(&v->ctx, v->segments[0]->url, in_fmt, NULL);

        SEAMLESS_LOG("read_header, after do avformat_open_input, ret: %d\n", ret);

        if (ret < 0)
            goto fail;

        v->stream_offset = stream_offset;
        v->ctx->ctx_flags &= ~AVFMTCTX_NOHEADER;
        ret = avformat_find_stream_info(v->ctx, NULL);
        if (ret < 0) {
            SEAMLESS_LOG("read_header, find stream info fail\n");
            goto fail;
        }

        SEAMLESS_LOG("read_header, stream num: %d\n", v->ctx->nb_streams);

        snprintf(bitrate_str, sizeof(bitrate_str), "%d", v->bandwidth);
        /* Create new AVStreams for each stream in this variant */
        for (j = 0; j < v->ctx->nb_streams; j++) {
            SEAMLESS_LOG("now new stream num: %d\n", j);

            AVStream *st = avformat_new_stream(s, NULL);
            if (!st) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            st->id = i;
            if (v->ctx && v->ctx->iformat &&
                    (!strcmp(v->ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") ||
                        !strcmp(v->ctx->iformat->name, "flv"))) {
                st->time_base.num = v->ctx->streams[j]->time_base.num;
                st->time_base.den = v->ctx->streams[j]->time_base.den;
            }

            avcodec_copy_context(st->codec, v->ctx->streams[j]->codec);
            if (v->bandwidth)
                av_dict_set(&st->metadata, "variant_bitrate", bitrate_str,
                                 0);
        }
        stream_offset += v->ctx->nb_streams;
  //  }

    c->first_packet = 1;
    c->first_timestamp = AV_NOPTS_VALUE;
    c->seek_timestamp  = AV_NOPTS_VALUE;
    c->seek_origin_timestamp = AV_NOPTS_VALUE;

    SEAMLESS_LOG("read_header out\n");
    return 0;
fail:
    free_variant_list(c);
    return ret;
}

static int recheck_discard_flags(AVFormatContext *s, int first)
{
    SeamContext *c = s->priv_data;
    int i, changed = 0;

    /* Check if any new streams are needed */
    for (i = 0; i < c->n_variants; i++)
        c->variants[i]->cur_needed = 0;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        struct SeamVariant *var = c->variants[s->streams[i]->id];
        if (st->discard < AVDISCARD_ALL)
            var->cur_needed = 1;
    }
    for (i = 0; i < c->n_variants; i++) {
        struct SeamVariant *v = c->variants[i];
        if (v->cur_needed && !v->needed) {
            v->needed = 1;
            changed = 1;
            v->cur_seq_no = c->cur_seq_no;
            v->pb.eof_reached = 0;
            av_log(s, AV_LOG_INFO, "Now receiving variant %d\n", i);
        } else if (first && !v->cur_needed && v->needed) {
            if (v->input)
                ffurl_close(v->input);
            v->input = NULL;
            v->needed = 0;
            changed = 1;
            av_log(s, AV_LOG_INFO, "No longer receiving variant %d\n", i);
        }
    }
    return changed;
}

static int check_update_base_time(struct SeamVariant* v, AVPacket *pkt)
{
    if ((v ==NULL) || (pkt == NULL)) {
        return 0;
    }

    if (pkt->stream_index >=RK_SEAM_MAX_STREAMS) {
        /* invalid stream index */
        return 0;
    }

    int32_t* jumpCnt = &v->upCtx[pkt->stream_index].haveJumpCnt;
    if ((pkt->pts == AV_NOPTS_VALUE) && (pkt->dts == AV_NOPTS_VALUE)) {
        if (((*jumpCnt)++) >=10) {
            *jumpCnt =0;
            return 1;
        } else {
            return 0;
        }
    }

    int64_t deltaTime = 0;
    int64_t* prePts = &v->upCtx[pkt->stream_index].prePktPts;
    int64_t* preDts = &v->upCtx[pkt->stream_index].prePktDts;
    int64_t* threshold = &v->upCtx[pkt->stream_index].segThreshold;

    if (pkt->pts != AV_NOPTS_VALUE) {
        if (*prePts ==0) {
            *prePts = pkt->pts;
        } else {
            deltaTime = abs(pkt->pts - *prePts);
        }
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        if (*preDts ==0) {
            *preDts = pkt->dts;
        } else {
            if (deltaTime ==0) {
                deltaTime = abs(pkt->dts - *preDts);
            }
        }
    }

    /*SEAMLESS_LOG("stream: %d, deltaTime: %lld, segThreshold: %lld",
        pkt->stream_index, deltaTime, *threshold);*/

    if (deltaTime && (deltaTime >(*threshold))) {
        SEAMLESS_LOG("segment time jump cnt: %d, delta: %lld",
            *jumpCnt, deltaTime);
        (*jumpCnt)++;
        if ((*jumpCnt) >=RK_SEAM_SEG_UPDATE_THRD) {
            *jumpCnt =0;
            *prePts =0;
            *preDts =0;
            v->upCtx[pkt->stream_index].forceUpdateInSeek = 0;
            return 1;
        }
    } else {
        *jumpCnt ==0;
    }

    if (*jumpCnt ==0) {
        if (pkt->pts != AV_NOPTS_VALUE) {
            *prePts = pkt->pts;
        }

        if (pkt->dts != AV_NOPTS_VALUE) {
            *preDts = pkt->dts;
        }
    }

    return 0;
}

static int seamless_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SeamContext *c = s->priv_data;
    int ret, i, minvariant = -1;

    if (c->first_packet) {
        recheck_discard_flags(s, 1);
        c->first_packet = 0;
    }

start:
    c->end_of_segment = 0;
   // for (i = 0; i < c->n_variants; i++) {
        i = 0;
        struct SeamVariant *var = c->variants[i];

        /* Make sure we've got one buffered packet from each open variant
         * stream */
        if (var->needed && !var->pkt.data) {
            while (1) {
                int64_t ts_diff;
                AVStream *st;
                ret = av_read_frame(var->ctx, &var->pkt);

                if (ret < 0) {
                    SEAMLESS_LOG("read packet, read frame fail, readPktErrCnt: %d, ret: %d\n",
                        var->readPktErrCnt, ret);
                    if (!url_feof(&var->pb) && ret != AVERROR_EOF){
                        SEAMLESS_LOG("seamless_read_packet fail, ret: %d, AVERROR(): %d, EAGAIN: %d\n",
                            ret, AVERROR(EAGAIN), EAGAIN);
                        if ((ret ==AVERROR(EAGAIN)) &&
                            (!strcmp(var->ctx->iformat->name, "flv"))) {
                            if (var->readPktErrCnt <RK_SEAM_FLV_READ_FAIL_TOLERAT_THRD) {
                                var->readPktErrCnt++;
                                reset_packet(&var->pkt);
                                SEAMLESS_LOG("continue\n");
                                continue;
                            } else {
                                SEAMLESS_LOG("read packet, readPktErrCnt beyonds threshold\n");
                                var->readPktErrCnt =0;
                            }
                        }

                        SEAMLESS_LOG("read packet return err: %d\n", ret);
                        return ret;
                    }
                    SEAMLESS_LOG("seamless_read_packet AVERROR_EOF ret = %d\n",ret);

                    reset_packet(&var->pkt);
                    break;
                } else {
                    if (c->first_timestamp == AV_NOPTS_VALUE)
                        c->first_timestamp = var->pkt.dts;
                }

                if (c->seek_timestamp == AV_NOPTS_VALUE)
                    break;

                if (var->pkt.dts == AV_NOPTS_VALUE) {
                    c->seek_timestamp = AV_NOPTS_VALUE;
                    c->seek_origin_timestamp = AV_NOPTS_VALUE;
                    break;
                }

                st = var->ctx->streams[var->pkt.stream_index];
                ts_diff = av_rescale_rnd(var->pkt.dts, AV_TIME_BASE,
                                         st->time_base.den, AV_ROUND_DOWN) -
                          c->seek_timestamp;

                if ((var->newUrlAfterSeek & HAVE_OPEN_URL) &&
                        (!(var->newUrlAfterSeek & HAVE_DONE_SUB_FORMAT_SEEK))) {
                    int64_t seekTimeUs = c->seek_timestamp;
                    seekTimeUs = (seekTimeUs)/(1000000ll*av_q2d(st->time_base));
                    int64_t curPos = avio_tell(var->ctx->pb);
                    int seekRet = 0;
                    int flags =0;

                    SEAMLESS_LOG("call av_seek_frame for new url after seek, pkt size: %d, index: %d\n",
                        var->pkt.size, var->pkt.stream_index);

                    if (!strcmp(var->ctx->iformat->name, "flv")) {
                        flags |=AVSEEK_FLAG_BYTE;
                        seekRet = flv_read_seamless_seek(var->ctx,
                                var->pkt.stream_index, -1, seekTimeUs, INT64_MAX, flags);
                    } else {
                        if (var->ctx->iformat->read_seek) {
                            var->ctx->iformat->read_seek(var->ctx,
                                var->pkt.stream_index, seekTimeUs,
                                flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
                        }
                    }

                    var->newUrlAfterSeek |=HAVE_DONE_SUB_FORMAT_SEEK;

                    SEAMLESS_LOG("pb: 0x%X, new url seek to: %lld(us), %lld(StrmScale), ret: %d, curPos: %lld\n",
                        var->ctx->pb, c->seek_timestamp, seekTimeUs, seekRet, curPos);
                    continue;
                }

                SEAMLESS_LOG("ts_diff: %lld, stream_index: %d, pts: %lld, dts: %lld, "
                    "seek_timestamp: %lld, var->pkt.flags: 0x%X, base: %d/%d, curPos: %lld\n",
                    ts_diff, var->pkt.stream_index, var->pkt.pts, var->pkt.dts,
                    c->seek_timestamp, var->pkt.flags, st->time_base.num, st->time_base.den,
                    avio_tell(var->ctx->pb));

                if (ts_diff >=0 && (var->newUrlAfterSeek & HAVE_OPEN_URL) &&
                        (c->seek_flags  & AVSEEK_FLAG_ANY || var->pkt.flags & AV_PKT_FLAG_KEY)) {
                    c->seek_timestamp = AV_NOPTS_VALUE;
                    break;
                }
            }
        }

        var->newUrlAfterSeek =0;

        /* Check if this stream has the packet with the lowest dts */
        if (var->pkt.data) {
            if(minvariant < 0) {
                minvariant = i;
            } else {
                struct SeamVariant *minvar = c->variants[minvariant];
                int64_t dts    =    var->pkt.dts;
                int64_t mindts = minvar->pkt.dts;
                AVStream *st   =    var->ctx->streams[   var->pkt.stream_index];
                AVStream *minst= minvar->ctx->streams[minvar->pkt.stream_index];

                if(   st->start_time != AV_NOPTS_VALUE)    dts -=    st->start_time;
                if(minst->start_time != AV_NOPTS_VALUE) mindts -= minst->start_time;

                if (av_compare_ts(dts, st->time_base, mindts, minst->time_base) < 0)
                    minvariant = i;
            }
        }
   // }
    if (c->end_of_segment) {
        if (recheck_discard_flags(s, 0)){
            goto start;
         }
    }
    /* If we got a packet, return it */
    if (minvariant >= 0) {
        AVStream *st = NULL;
        if (var && (var->pkt.stream_index >=0) &&
                (var->pkt.stream_index <var->ctx->nb_streams)) {
            /* save segment base time, avoid calculating it every time */
            if (var->pkt.stream_index <RK_SEAM_MAX_STREAMS) {
                int64_t* segBase = &var->upCtx[var->pkt.stream_index].segBaseTimeStrmScale;
                int64_t* segHold = &var->upCtx[var->pkt.stream_index].segThreshold;
                st = var->ctx->streams[var->pkt.stream_index];

                if (*segHold ==0) {
                    *segHold =(5000000ll)/(1000000ll*av_q2d(st->time_base));   /* 5s */
                }

                if (check_update_base_time(var, &var->pkt) ||
                        var->upCtx[var->pkt.stream_index].forceUpdateInSeek) {
                    *segBase =(var->segBaseTimeUs)/(1000000ll*av_q2d(st->time_base));
                    SEAMLESS_LOG("update segBaseTimeUs to: %lld", *segBase);
                }

                c->variants[minvariant]->pkt.pts +=(*segBase);
                c->variants[minvariant]->pkt.dts +=(*segBase);
            }
        }

        *pkt = c->variants[minvariant]->pkt;
        pkt->stream_index += c->variants[minvariant]->stream_offset;
        reset_packet(&c->variants[minvariant]->pkt);

        if (pkt && (pkt->stream_index >=0) &&
                (pkt->stream_index <RK_SEAM_MAX_STREAMS) &&
                (var->drop_audio[var->pkt.stream_index].drop_cnt) &&
                st && st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* drop some audio packet after open new url */
            pkt->stream_index = -1;
            av_free_packet(pkt);
            var->drop_audio[var->pkt.stream_index].drop_cnt--;

            SEAMLESS_LOG("drop one audio packet, left %d packet(s) to drop\n",
                var->drop_audio[var->pkt.stream_index].drop_cnt);
        }

        return 0;
    }

    av_log(NULL, AV_LOG_ERROR, "Seamless read AVERROR_EOF\n");
    return AVERROR_EOF;
}

static int seamless_close(AVFormatContext *s)
{
    SeamContext *c = s->priv_data;

    free_variant_list(c);
    return 0;
}

static int seamless_read_seek(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    SeamContext *c = s->priv_data;
    int i, j, ret;
    i = 0;
    struct SeamVariant *var = c->variants[i];

    SEAMLESS_LOG("seek to: %lld, flags: %d, streamIdx: %d, newUrlAfterSeek: 0x%X\n",
        timestamp, flags, stream_index, var->newUrlAfterSeek);

    if ((flags & AVSEEK_FLAG_BYTE) || !c->variants[0]->finished)
        return AVERROR(ENOSYS);

    if (var->newUrlAfterSeek) {
        SEAMLESS_LOG("we are current in seek now, skip this seek\n");
        return 0;
    }

    c->seek_flags     = flags;
    c->seek_timestamp = stream_index < 0 ? timestamp :
                        av_rescale_rnd(timestamp, AV_TIME_BASE,
                                       s->streams[stream_index]->time_base.den,
                                       flags & AVSEEK_FLAG_BACKWARD ?
                                       AV_ROUND_DOWN : AV_ROUND_UP);
    timestamp = av_rescale_rnd(timestamp, 1, stream_index >= 0 ?
                               s->streams[stream_index]->time_base.den :
                               AV_TIME_BASE, flags & AVSEEK_FLAG_BACKWARD ?
                               AV_ROUND_DOWN : AV_ROUND_UP);
    if (s->duration < c->seek_timestamp) {
        c->seek_timestamp = s->duration;
    }

    c->seek_origin_timestamp = c->seek_timestamp;

    ret = AVERROR(EIO);
   // for (i = 0; i < c->n_variants; i++) {
        /* Reset reading */
        var->newUrlAfterSeek |=HAVE_SEEK_FLAG;
        double pos = c->first_timestamp == AV_NOPTS_VALUE ? 0 :
                      av_rescale_rnd(c->first_timestamp, 1, stream_index >= 0 ?
                               s->streams[stream_index]->time_base.den :
                               AV_TIME_BASE, flags & AVSEEK_FLAG_BACKWARD ?
                               AV_ROUND_DOWN : AV_ROUND_UP);

        if (var->input) {
            ffurl_close(var->input);
            var->input = NULL;
        }

        av_free_packet(&var->pkt);
        reset_packet(&var->pkt);
        var->pb.eof_reached = 0;
        /* Clear any buffered data */
        var->pb.buf_end = var->pb.buf_ptr = var->pb.buffer;
        /* Reset the pos, to let the mpegts demuxer know we've seeked. */
        var->pb.pos = 0;

        /* Locate the segment that contains the target timestamp */
        for (j = 0; j < var->n_segments; j++) {
            if (timestamp >= pos &&
                timestamp < pos + var->segments[j]->duration) {
                var->cur_seq_no = var->start_seq_no + j;
                ret = 0;
                break;
            }
            pos += var->segments[j]->duration;
        }
        if(j == var->n_segments){
              var->cur_seq_no = var->start_seq_no + j;
              ret = 0;
        }
        if (ret)
            c->seek_timestamp = AV_NOPTS_VALUE;
    //}
    if(pos >=0){
		c->seek_timestamp -= ((int64_t)pos)*1000000;
	}else{
        c->seek_timestamp = timestamp * 1000000ll;
    }

    for (i =0; i <var->ctx->nb_streams; i++) {
        var->upCtx[i].forceUpdateInSeek = 1;
    }

    flush_avio_buffer(var);
    reset_demuxer_if_necessary(var);

    return ret;
}

static int seamless_probe(AVProbeData *p)
{
    SEAMLESS_LOG("seamless_probe in\n");

    /* Require #ROCKCHIPPLAY at the start, and either one of the ones below
     * somewhere for a proper match. */
    if (strncmp(p->buf, RK_SEAMLESS_IDENTIFIER_TAG, strlen(RK_SEAMLESS_IDENTIFIER_TAG)))
        return 0;
    if (strstr(p->buf, RK_SEAMLESS_IDENTIFIER_TAG)     ||
        strstr(p->buf, RK_SEAMLESS_STREAM)) {
        SEAMLESS_LOG("seamless probe pass\n");
        return AVPROBE_SCORE_MAX;
    }
    return 0;
}

AVInputFormat ff_seamless_demuxer = {
    .name           = "rockchip_seamless",
    .long_name      = NULL_IF_CONFIG_SMALL("RockChip Seamless Streaming"),
    .priv_data_size = sizeof(SeamContext),
    .read_probe     = seamless_probe,
    .read_header    = seamless_read_header,
    .read_packet    = seamless_read_packet,
    .read_close     = seamless_close,
    .read_seek      = seamless_read_seek,
};
