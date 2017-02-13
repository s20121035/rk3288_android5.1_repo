/*
 * RKPcm demuxer
 * Copyright (c) 2003 The FFmpeg Project
 *
 * This demuxer will generate a 1 byte extradata for VP6F content.
 * It is composed of:
 *  - upper 4bits: difference between encoded width and visible width
 *  - lower 4bits: difference between encoded height and visible height
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

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libavutil/intfloat.h"
#include "libavutil/mathematics.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/mpeg4audio.h"
#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"
#include "flv.h"

#define RKPCM_DEBUG 0

#if RKPCM_DEBUG
#define RKPCM_LOG(...)   av_log(NULL, AV_LOG_INFO, __VA_ARGS__)
#else
#define RKPCM_LOG(...)   do { if (0) av_log(NULL, AV_LOG_INFO, __VA_ARGS__); } while (0)
#endif

#define VALIDATE_INDEX_TS_THRESH 2500
#define FLV_TAG 0x464C5601      //.pcm

typedef struct {
    int64_t filesize;
    int xing_toc;
    int start_pad;
    int end_pad;
} RKPCMContext;
static int rkpcm_probe(AVProbeData *p)
{
    const uint8_t *d;
    d = p->buf;
    int score = 0;
	#if 1
	int i;
	for(i = 0; i < p->buf_size; i++){
		if(d[i] != 0)
			break;
	}
   
    
	if(i == p->buf_size && (i != 0)){
         av_log(NULL, AV_LOG_ERROR,"Hery, probe p->buf_size = %d", p->buf_size);
		score = AVPROBE_SCORE_MAX;
	}else{
		score = 0;
	}
	#endif
    av_log(NULL, AV_LOG_ERROR,"Hery, probe size = %d", i);
    //return score;
    return 0;
}

static AVStream *create_stream(AVFormatContext *s, int codec_type)
{
	RKPCMContext *rkpcm = s->priv_data;
    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return NULL;
    st->codec->codec_type = codec_type;
	st->codec->codec_id = AV_CODEC_ID_FIRST_AUDIO;
	st->codec->channels = 2;
	st->codec->sample_rate = 44100;
	st->need_parsing = AVSTREAM_PARSE_FULL_RAW;
	st->start_time = 0;
	avpriv_set_pts_info(st, 32, 1, 1000); /* 32 bit pts in ms */
    return st;
}

static int rkpcm_read_header(AVFormatContext *s)
{
	av_log(NULL, AV_LOG_ERROR, "Hery, rkpcm_read_header");
	create_stream(s, AVMEDIA_TYPE_AUDIO);
    s->start_time = 0;
    return 0;
}

static int rkpcm_read_close(AVFormatContext *s)
{
	av_log(NULL, AV_LOG_ERROR, "Hery,  rkpcm_read_close");
    return 0;
}

static int rkpcm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
	int size = 4096;
	int ret= av_get_packet(s->pb, pkt, size);
    if (ret < 0){
		av_log(NULL, AV_LOG_ERROR, "Hery, return ret = %d", ret);
        return ret;
    }
    pkt->size = ret;
    return 0;
}

static int rkpcm_read_seek(AVFormatContext *s, int stream_index,
    int64_t ts, int flags)
{
	int ret ;
	AVIOContext * pcmtest = s->pb;
	URLContext * h = pcmtest->opaque;
	int64_t totalsize = avio_size(s->pb);
	int seek_time = ts / 1000;
	int seek_pos = (seek_time * totalsize )/ 60;
	seek_pos = (seek_pos / 4096) * 4096;
	ret = h->prot->url_seek(h, seek_pos, SEEK_SET);
	av_log(NULL, AV_LOG_ERROR, "Hery, rkpcm_read_seek ret = %d", ret);
    return 0;
}

#define OFFSET(x) offsetof(RKPCMContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
	{ NULL }
};

static const AVClass class = {
    .class_name = "rkpcmdec",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_rkpcm_demuxer = {
    .name           = "rk, pcm",
    .long_name      = NULL_IF_CONFIG_SMALL("RKPCM (RK PCM Audio)"),
    .priv_data_size = sizeof(RKPCMContext),
    .read_probe     = rkpcm_probe,
    .read_header    = rkpcm_read_header,
    .read_packet    = rkpcm_read_packet,
    .read_seek      = rkpcm_read_seek,
    .read_close     = rkpcm_read_close,
    .extensions     = "rkpcm",
    .priv_class     = &class,
};
