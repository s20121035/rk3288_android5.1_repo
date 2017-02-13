/*
 * Apple HTTP Live Streaming demuxer
 * Copyright (c) 2010 Martin Storsjo
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
 * Apple HTTP Live Streaming demuxer
 * http://tools.ietf.org/html/draft-pantos-http-live-streaming
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

#define INITIAL_BUFFER_SIZE 32768
#define HLS_DEBUG 1
#define QVOD_NEED 0

#define DOWNLOAD_M3U8_START -1
#define DOWNLOAD_M3U8_END -1

#define TIMEOUT 2000

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

enum KeyType {
    KEY_NONE,
    KEY_AES_128,
};

//add by xhr, for QVOD  AV async
typedef enum distontinutystatus {
    Distontinuty_NONE,
    Distontinuty_ONE,
    Distontinuty_ALL,
}DistontinutyStatus;


struct segment {
    double duration;
    int discontinuity;
	int is_seek;           //add by xhr, for Bluray
	double seek_time;	   //add by xhr, for Bluray
	double seek_time_end;  //add by xhr, for Bluray
	double seek_operation_time;  //add by xhr, for Bluray
    char url[MAX_URL_SIZE];
    char key[MAX_URL_SIZE];
    enum KeyType key_type;
    uint8_t iv[16];
};
struct bandwidth_info{
    int bandwidth;
    char url[MAX_URL_SIZE];
};
typedef struct bandwidthcontext{
	int64_t mtime;
	int32_t msize;
	struct bandwidthcontext *next;
}BandWidthContext;

typedef struct bandWidthqueue{
	BandWidthContext *front;
	BandWidthContext *rear;
	int num;
    int64_t mTotalSize;
    int64_t mTotalTimeUs;
}BandWidthQueue;

/*
 * Each variant has its own demuxer. If it currently is active,
 * it has an open AVIOContext too, and potentially an AVPacket
 * containing the next packet from this stream.
 */
struct variant {
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
    struct segment **segments;
    int needed, cur_needed;
    int cur_seq_no;
    int filesize;
    int64_t load_time;
    int64_t last_load_time;

    int64_t ts_send_time;
    int64_t ts_first_time;
    int64_t ts_last_time;

    char key_url[MAX_URL_SIZE];
    uint8_t key[16];
};

typedef struct HLSContext {
    int n_variants;
    int n_bandwidth_info;
    struct variant **variants;
    struct bandwidth_info **bandwidth_info;
    BandWidthQueue *BandWidthqueue;
    char *cookies;   //add by xhr, for set cookies
    int cur_seq_no;
    int end_of_segment;
    int first_packet;
    int64_t first_timestamp;
    int64_t seek_timestamp;
    int seek_flags;
    int abort;
	int mTaobaoHevc;    //add by xhr, for QVOD AV async
	int mdynamic;
    int mSort;      //add by ht ,1 save min bandwidth rate; 0,default save max bandwidth rate
    int misHeShiJie;
    AVIOInterruptCB *interrupt_callback;
    char header[MAX_URL_SIZE];
    int read_seq_no;
} HLSContext;
BandWidthQueue* InitQueue();  
int IsEmpty(BandWidthQueue *pqueue);  
BandWidthContext* DeQueue(BandWidthQueue *pqueue);  
void EnQueue(BandWidthQueue *pqueue,int32_t size, int64_t time); 
void ClearQueue(BandWidthQueue *pqueue);  
void DestroyQueue(BandWidthQueue *pqueue);  
int GetSize(BandWidthQueue *pqueue);  
int EstimateBandwidth(HLSContext *c,int32_t *);
int SortBandwidth(HLSContext* c);

BandWidthQueue* InitQueue()
{
	BandWidthQueue* pqueue = (BandWidthQueue*)av_mallocz(sizeof(BandWidthQueue));
	if(pqueue != NULL)
	{
		pqueue->front = NULL;
		pqueue->rear = NULL;
		pqueue->num = 0;
        pqueue->mTotalSize = 0;
        pqueue->mTotalTimeUs = 0;
	}
	return pqueue;
}
int IsEmpty(BandWidthQueue *pqueue)
{
    if(pqueue == NULL)
    {
        av_log(NULL,AV_LOG_DEBUG,"IsEmpty:something err");
        return 1;
    }    
	if(pqueue->front == NULL && pqueue->rear == NULL && pqueue->num == 0)
	{
		return 1;
	}else{
		return 0;
	}
}
BandWidthContext *  DeQueue(BandWidthQueue *pqueue)  
{  
    if(pqueue == NULL)
    {
        av_log(NULL,AV_LOG_DEBUG,"DeQueue:something err");
        return NULL;
    }
    BandWidthContext* pnode = pqueue->front;  
    if(IsEmpty(pqueue)!=1 && pnode!=NULL)  
    {  
        pqueue->num--;  
        pqueue->front = pnode->next;  
        pqueue->mTotalSize -= pnode->msize;
        pqueue->mTotalTimeUs -= pnode->mtime;
        av_free(pnode);  
        if(pqueue->num==0)  
            pqueue->rear = NULL;  
    }  
	return pqueue->front;
} 
void  EnQueue(BandWidthQueue *pqueue, int32_t size, int64_t time)  
{  
    if(pqueue == NULL)
    {
        av_log(NULL,AV_LOG_DEBUG,"EnQueue:something err");
        return;
    }
    BandWidthContext * pnode = (BandWidthContext *)av_mallocz(sizeof(BandWidthContext));  
    if(pnode != NULL)  
    {  
        pnode->msize = size;  
		pnode->mtime = time;  
        pnode->next = NULL;  
        if(IsEmpty(pqueue))  
        {  
            pqueue->front = pnode;  
        }  
        else  
        {  
            pqueue->rear->next = pnode;  
        }  
        pqueue->rear = pnode;  
        pqueue->num++; 
        pqueue->mTotalSize += pnode->msize;
        pqueue->mTotalTimeUs += pnode->mtime;
    }
    if (pqueue->num > 100)
    {
        DeQueue(pqueue);
    }    
}  
void ClearQueue(BandWidthQueue *pqueue)
{
	while(IsEmpty(pqueue) != 0){
		DeQueue(pqueue);
	}
}
void DestroyQueue(BandWidthQueue * pqueue)
{
	if(IsEmpty(pqueue) != 1)
		ClearQueue(pqueue);
    if(pqueue != NULL)
	av_free(pqueue);
}
int GetSize(BandWidthQueue *pqueue)  
{  
    if(pqueue == NULL)
    {
        av_log(NULL,AV_LOG_DEBUG,"GetSize:something err");
        return 0;
    }    
    return pqueue->num;  
}  
int EstimateBandwidth(HLSContext *c,int32_t *bandwidth_bps)
{
        if(GetSize(c->BandWidthqueue) >= 2)
        {
        
            av_log(NULL, AV_LOG_DEBUG, "c->Bandwidthqueue->mTotalSize = %lld", c->BandWidthqueue->mTotalSize);
            av_log(NULL, AV_LOG_DEBUG, "c->Bandwidthqueue->mTotalTimeUs = %lld", c->BandWidthqueue->mTotalTimeUs);
        
            *bandwidth_bps = ((c->BandWidthqueue->mTotalSize * 8000000ll)/c->BandWidthqueue->mTotalTimeUs);
            return 1;

        }
        return 0;
}
int SortBandwidth(HLSContext* c)
{
    int i,j,h;
    int tmp = 0;
    char* ptr = (char*)av_malloc(MAX_URL_SIZE+1);
    
    for(i=0;i<c->n_bandwidth_info-1;i++)
    {
        for(j=0;j<c->n_bandwidth_info-1-i;j++)
        {
            if(c->bandwidth_info[j]->bandwidth > c->bandwidth_info[j+1]->bandwidth)
            {
                tmp = c->bandwidth_info[j]->bandwidth;
                c->bandwidth_info[j]->bandwidth = c->bandwidth_info[j+1]->bandwidth;
                c->bandwidth_info[j+1]->bandwidth = tmp;

                memcpy(ptr,c->bandwidth_info[j]->url,MAX_URL_SIZE);
                memcpy(c->bandwidth_info[j]->url,c->bandwidth_info[j+1]->url,MAX_URL_SIZE);
                memcpy(c->bandwidth_info[j+1]->url,ptr,MAX_URL_SIZE);
            }
        }
    }

    for(h=0;h<c->n_bandwidth_info;h++)
    {
        av_log(NULL,AV_LOG_DEBUG,"h=%d",h);
        av_log(NULL,AV_LOG_DEBUG,"***bandwidth=%d",c->bandwidth_info[h]->bandwidth);
        av_log(NULL,AV_LOG_DEBUG,"***url=%s",c->bandwidth_info[h]->url);
    }
    
    av_free(ptr);
    ptr = NULL;

}

static int read_chomp_line(AVIOContext *s, char *buf, int maxlen)
{
    int len = ff_get_line(s, buf, maxlen);
    while (len > 0 && isspace(buf[len - 1]))
        buf[--len] = '\0';
    return len;
}

static void free_segment_list(struct variant *var)
{
    int i;
    for (i = 0; i < var->n_segments; i++)
        av_free(var->segments[i]);
    av_freep(&var->segments);
    var->n_segments = 0;
}

static void free_variant_list(HLSContext *c)
{
    int i;
    for (i = 0; i < c->n_variants; i++) {
        struct variant *var = c->variants[i];
        free_segment_list(var);
        av_free_packet(&var->pkt);
        av_free(var->pb.buffer);
        if (var->input)
            ffurl_close(var->input);
        if (var->ctx) {
            var->ctx->pb = NULL;
            avformat_close_input(&var->ctx);
        }
        av_free(var);
    }
    for(i=0;i<c->n_bandwidth_info;i++)
    {
        struct bandwidth_info * tmp = c->bandwidth_info[i];
        av_free(tmp);
    }
    av_freep(&c->variants);
    av_freep(&c->bandwidth_info);
    c->n_variants = 0;
    c->n_bandwidth_info = 0;
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

static struct variant *new_variant(HLSContext *c, int bandwidth,
                                   const char *url, const char *base)
{
    struct variant *var = av_mallocz(sizeof(struct variant));
    struct bandwidth_info *tmp = av_mallocz(sizeof(struct bandwidth_info));
    if (!var && !tmp)
        return NULL;
    reset_packet(&var->pkt);
    var->bandwidth = bandwidth;
    tmp->bandwidth = bandwidth;
    ff_make_absolute_url(var->url, sizeof(var->url), base, url);
    ff_make_absolute_url(tmp->url, sizeof(tmp->url), base, url);
    av_log(NULL,AV_LOG_DEBUG,"new_variant:%s",var->url);
    dynarray_add(&c->variants, &c->n_variants, var);
    dynarray_add(&c->bandwidth_info,&c->n_bandwidth_info, tmp);
    return var;
}

struct variant_info {
    char bandwidth[20];
};

static void handle_variant_args(struct variant_info *info, const char *key,
                                int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "BANDWIDTH=", key_len)) {
        *dest     =        info->bandwidth;
        *dest_len = sizeof(info->bandwidth);
    }
}

struct key_info {
     char uri[MAX_URL_SIZE];
     char method[10];
     char iv[35];
};

static void handle_key_args(struct key_info *info, const char *key,
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

static int parse_playlist(HLSContext *c, const char *url,
                          struct variant *var, AVIOContext *in)
{
    //Use redirect url
    AVIOParams *ioparams = c->interrupt_callback->ioparams;
    if(ioparams&&ioparams->type==1&&ioparams->redirect==1&&ioparams->url[0]!='\0') {
        url = ioparams->url;
    }
#if HLS_DEBUG
    av_log(NULL, AV_LOG_DEBUG,"HLS parse_playlist in, url = %s\n", url);
#endif

	DistontinutyStatus distontinuty_status = Distontinuty_NONE; //add by xhr, for QVOD
	c->mTaobaoHevc = 0;  //add by xhr, for taobao hevc 0x6
    int ret = 0,is_segment = 0,is_discontinuity = 0, is_variant = 0, bandwidth = 0;
    int is_seek = 0;				//add by xhr, for Bluray
	double seek_time = 0;			//add by xhr, for Bluray
	double seek_time_end = 0;		//add by xhr, for Bluray
    double duration = 0;
    enum KeyType key_type = KEY_NONE;
    uint8_t iv[16] = "";
    int has_iv = 0;
    char key[MAX_URL_SIZE] = "";
    char line[1024];
    const char *ptr;
    int close_in = 0;
    if (!in) {
        AVDictionary *opts = NULL;
        close_in = 1;
        /* Some HLS servers dont like being sent the range header */
        av_dict_set(&opts, "seekable", "0", 0);
		if(c->cookies){
			av_dict_set(&opts, "cookies", c->cookies, 0);
		}
//        av_dict_set(&opts, "hls", "1", 0); // ht modified for hls timeout 500ms; other is 60s
        av_dict_set(&opts, "timeout", "5000000", 0);
        av_dict_set(&opts, "multiple_requests", "0", 0);
        av_dict_set(&opts, "hls_parse", "1", 0);
        if(strlen(c->header) > 0)
        {
            av_dict_set(&opts, "headers", c->header, 0);
        }

        //Init http persist connection params
        if(c->misHeShiJie==1&&!c->interrupt_callback->ioparams){
            c->interrupt_callback->ioparams = av_malloc(sizeof(struct AVIOParams));
            AVIOParams *params = c->interrupt_callback->ioparams;
            if(params) {
                params->host[0] = '\0';
                params->port = -1;
                params->hd = NULL;
                params->type = 1;
                params->willclose = 1;
                params->url[0] = '\0';
                params->redirect = 0;
            }
        }

        // not set user-agent here, set user-agent in ffplayer or use default user-agent in http_connect
        // AppleCoreMedia/1.0.0.7B367 (iPad; U; CPU OS 4_3_3 like Mac OS X)
//        av_dict_set(&opts, "user-agent","Mozilla/4.0 (compatible; MS IE 6.0; (ziva))",0);//"stagefright/1.2 (Linux;Android 4.2.2)"
        ret = avio_open2(&in, url, AVIO_FLAG_READ,
                         c->interrupt_callback, &opts);
        av_log(NULL,AV_LOG_ERROR,"%s:avio_open2 ",__FUNCTION__);
        av_dict_free(&opts);
        if (ret < 0)
        {
#if HLS_DEBUG
            av_log(NULL,AV_LOG_DEBUG,"%s:avio_open failed ret=0x%x",__FUNCTION__,ret);
#endif
            if(ioparams&&ioparams->type==1)
            {
                ioparams->url[0] = '\0';
                ioparams->redirect = 0;
            }
            return ret;
        }
    }
#if HLS_DEBUG
            av_log(NULL,AV_LOG_DEBUG,"%s:avio_open ok",__FUNCTION__,ret);
#endif

    read_chomp_line(in, line, sizeof(line));
    if (strcmp(line, "#EXTM3U")) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    struct variant *bkVar = NULL;
    /* Disable merge m3u8
    if(var&&var->finished==0&&var->n_segments>0){//Http Living Streaming
        bkVar = var;
        var = av_mallocz(sizeof(struct variant));//new_variant(c, 0, in->url, NULL);
        ff_make_absolute_url(var->url, sizeof(var->url), NULL, in->url);
    }*/
     
    if (var) {
        free_segment_list(var);
        var->finished = 0;
    }
    while (!url_feof(in)) {
		//add by xhr ,for prevent m3u8 too big lead full leap
		if(var && var->n_segments >=4096)
		{
			break;
		}
        read_chomp_line(in, line, sizeof(line));
//        av_log(NULL,AV_LOG_ERROR,"%s",line);

        /*
        * 如果read_chomp_line读失败，将导致http_buf_read调用special_read
        * 这种情况下，会重新向服务器申请m3u8表，导致一次读到多个m3u8表的情况
        * 不同的m3u8表可能带有同一个sequence，因此可能会导致某个ts片段播放多次的情况
        * 因此，这边需要判断是否有新的m3u8表进来，如果有的话只保存最好一个m3u8表
        */
        if (strstr(line, "#EXTM3U"))
        {
            if (var != NULL) 
            {
                free_segment_list(var);
                var->finished = 0;
            }
        }
        
        if (av_strstart(line, "#EXT-X-STREAM-INF:", &ptr)) {
            struct variant_info info = {{0}};
            is_variant = 1;
#if HLS_DEBUG
            av_log(NULL, AV_LOG_DEBUG,"EXT-X-STREAM-INF\n");
#endif
            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_variant_args,
                               &info);
            bandwidth = atoi(info.bandwidth);
			av_log(NULL,AV_LOG_DEBUG,"bandwidth=%d",bandwidth);
        } else if (av_strstart(line, "#EXT-X-KEY:", &ptr)) {
            struct key_info info = {{0}};
            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_key_args,
                               &info);
            key_type = KEY_NONE;
            has_iv = 0;
            if (!strcmp(info.method, "AES-128"))
                key_type = KEY_AES_128;
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
		   //add by xhr, for QVOD  AV async
		   if(distontinuty_status == Distontinuty_NONE){
				distontinuty_status = Distontinuty_ONE;
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
        } else if (av_strstart(line, "#EXT-X-ENDLIST", &ptr)) {
            if (var)
                var->finished = 1;
        }
		else if(av_strstart(line, "#SLICE-CODEC:H.265", &ptr))
        {
        	av_log(NULL, AV_LOG_ERROR, "#SLICE-CODEC:H.265");
			c->mTaobaoHevc = 1;
        }
		else if (av_strstart(line, "#EXTINF", &ptr)) {
            char *temp = NULL;
            is_segment = 1;
            temp = strstr(ptr,":");

            ptr = temp+1;
            duration   = strtod(ptr,NULL);
        }else if(av_strstart(line, "#EXT-X-DISCONTINUITY",NULL)){
        	//add by xhr, for QVOD  AV async
           if(distontinuty_status == Distontinuty_NONE){
				distontinuty_status = Distontinuty_ALL;
		   }
           is_discontinuity = 1;
            av_log(NULL, AV_LOG_DEBUG,"HLS parse DISCONTINUITY\n");
        }else if(av_strstart(line, "#EXT-X-SEEK:",&ptr)) {     //add by xhr, for Bluray
        	is_seek = 1;
			seek_time = strtod(ptr,NULL);
			av_log(NULL, AV_LOG_DEBUG, "Hery, seek_position = %f", seek_time);
		}else if(av_strstart(line,"#EXT-X-SEEK-END:", &ptr)){
			seek_time_end = strtod(ptr,NULL);
			av_log(NULL, AV_LOG_DEBUG, "Hery, seek_position_end = %f", seek_time_end);
		} else if (av_strstart(line, "#", NULL)) {
            continue;
        } else if (line[0]) {
            if (is_variant) {
#if HLS_DEBUG
                av_log(NULL, AV_LOG_DEBUG,"url = %s\n",url);
#endif

          if(0 == c->mdynamic)
          {
                int tempMax = 0;
                int firstVariants = 0;
                if(c->n_variants == 0)
                {
                    tempMax = bandwidth;
                    firstVariants = 1;
                }

                for (int i = 0; i < c->n_variants; i++) {
                    struct variant *var = c->variants[i];

                    if(var != NULL)
                    {
                        if(c->mSort == 0)
                        {
                            if(bandwidth > var->bandwidth)
                            {
                                tempMax = bandwidth;
                                av_log(NULL,AV_LOG_DEBUG," change max =%d",tempMax);
                                free_variant_list(c);

                            }
                        }
                        else if(c->mSort == 1)
                        {
                            if(bandwidth < var->bandwidth)
                            {
                                tempMax = bandwidth;
                                av_log(NULL,AV_LOG_DEBUG," change min =%d",tempMax);
                                free_variant_list(c);

                            }

                        }
                    }

                }

                if(tempMax != 0 || 1 == firstVariants)
                {
                    if (!new_variant(c, tempMax, line, in->url)) 
                    {
                        
                        ret = AVERROR(ENOMEM);
                        goto fail;
                    } 
                }
          }
          else
          {

                if (!new_variant(c, bandwidth, line, in->url)) {
                    
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
          }
              
                is_variant = 0;
                bandwidth  = 0;
            }
            if (is_segment) {
                struct segment *seg;
                if (!var) {
                    var = new_variant(c, 0, in->url, NULL);
                    if (!var) {
                        ret = AVERROR(ENOMEM);
                        goto fail;
                    }
                }
                seg = av_malloc(sizeof(struct segment));
                if (!seg) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                seg->duration = duration;
                seg->key_type = key_type;
				seg->seek_operation_time = 0;  //add by xhr, for Bluray seek operation
                if(is_discontinuity){
                    seg->discontinuity = 1;
                    is_discontinuity = 0;
                }else{
                    seg->discontinuity = 0;
                }
				//add by xhr , for Bluray
				if(is_seek){
					seg->is_seek = 1;
					seg->seek_time = seek_time;
					seg->seek_time_end = seek_time_end;
					//seg->real_duration = real_duration;
					is_seek = 0;
					seek_time = 0;
					seek_time_end = 0;
					//real_duration = 0;
					duration= 0;
				}else{
					seg->is_seek = 0;
					seg->seek_time = 0;
					seg->seek_time_end = 0;
					//seg->real_duration = 0; 
					duration = 0;
				}
                if (has_iv) {
                    memcpy(seg->iv, iv, sizeof(iv));
                } else {
                    int seq = var->start_seq_no + var->n_segments;
                    memset(seg->iv, 0, sizeof(seg->iv));
                    AV_WB32(seg->iv + 12, seq);
                }
                ff_make_absolute_url(seg->key, sizeof(seg->key), in->url, key);
                ff_make_absolute_url(seg->url, sizeof(seg->url), in->url, line);
                dynarray_add(&var->segments, &var->n_segments, seg);
                is_segment = 0;
            }
        }
    }
    if (var){
        //Merge m3u8
        int m3u8_handled = 0;
        int last_bk_seq_no = bkVar?(bkVar->start_seq_no+bkVar->n_segments-1):0;
        if(!var->finished&&bkVar&&bkVar->cur_seq_no>=bkVar->start_seq_no&&bkVar->cur_seq_no<=last_bk_seq_no){ 
            av_log(NULL,AV_LOG_DEBUG, "we try to merge m3u8. ");
            if(var->start_seq_no-bkVar->cur_seq_no>0){//May be jump
                if(var->start_seq_no-last_bk_seq_no<=1){
                    int i=0, j=0;
                    //1.free used segments
                    av_log(NULL,AV_LOG_DEBUG,"free used segments: %d, %d - %d", bkVar->cur_seq_no, bkVar->start_seq_no, last_bk_seq_no);
                    if(bkVar->cur_seq_no>bkVar->start_seq_no){
                        for (i = 0; i < bkVar->n_segments; i++){
                            if((bkVar->start_seq_no+i) < bkVar->cur_seq_no){
                                av_free(bkVar->segments[i]);
                            }else{
                                bkVar->segments[j++] = bkVar->segments[i];
                                bkVar->segments[i] = NULL;
                            }
                        }
                        bkVar->start_seq_no = bkVar->cur_seq_no;
                        bkVar->n_segments = j;
                    }
                    //2.add new segments
                    int last_cur_seq_no = var->start_seq_no+var->n_segments-1;
                    av_log(NULL,AV_LOG_DEBUG,"add new segments: %d - %d", var->start_seq_no, last_cur_seq_no);
                    for (i = 0; i < var->n_segments; i++){
                        if((var->start_seq_no+i)<=last_bk_seq_no){
                            av_free(var->segments[i]);
                        }else if(bkVar->n_segments>50){//If we have store more then 50 segments, we need goto living
                            av_free(var->segments[i]);
                            av_log(NULL,AV_LOG_DEBUG,"we have already stored more then %d segments ", bkVar->n_segments);
                        }else{
                            dynarray_add(&bkVar->segments, &bkVar->n_segments, var->segments[i]);
                            var->segments[i] = NULL;
                        }
                    }
                    //3.set new something, then free
                    bkVar->target_duration = var->target_duration;
                    av_freep(&var->segments);
                    av_freep(&var);
                    var = bkVar;
                    
                    av_log(NULL,AV_LOG_DEBUG,"now segments: %d - %d", var->start_seq_no, var->start_seq_no+var->n_segments-1);
                    //for(i=0; i<var->n_segments; i++){
                    //    av_log(NULL,AV_LOG_DEBUG,"Merge m3u8: %s", var->segments[i]->url);
                    //}
                    
                    m3u8_handled = 1;
                }
            }
        }

        if(!m3u8_handled&&!var->finished&&bkVar){
            av_log(NULL,AV_LOG_DEBUG,"Recovery m3u8: %d - %d", var->start_seq_no, var->start_seq_no+var->n_segments-1);
            free_segment_list(bkVar);
            int i = 0;
            
            for(i = 0; i < var->n_segments; i++){
                dynarray_add(&bkVar->segments, &bkVar->n_segments, var->segments[i]);
                var->segments[i] = NULL;
            }
            bkVar->finished = var->finished;
            bkVar->start_seq_no = var->start_seq_no;
            bkVar->target_duration = var->target_duration;
            free_segment_list(var);
            av_freep(&var->segments);
            av_freep(&var);
            var = bkVar;
            av_log(NULL,AV_LOG_DEBUG,"Recovery m3u82: %d - %d", var->start_seq_no, var->start_seq_no+var->n_segments-1);
            for(i=0; i<var->n_segments; i++){
                //av_log(NULL,AV_LOG_DEBUG,"Merge m3u8: %s", var->segments[i]->url);
            }
        }
        
        var->last_load_time = av_gettime();
        
    }

#if HLS_DEBUG
    if(var)
    {

        av_log(NULL, AV_LOG_DEBUG,"%s::firstQ=%d,curQ=%d,LastQ=%d,var->n_segments = %d",__FUNCTION__,var->start_seq_no,var->cur_seq_no,var->start_seq_no+var->n_segments,var->n_segments);
    }

#endif

fail:
    if (close_in)
        avio_close(in);
    return ret;
}

static int open_input(HLSContext *c, struct variant *var)
{
    AVDictionary *opts = NULL;
    int ret;
    struct segment *seg = var->segments[var->cur_seq_no - var->start_seq_no];
    av_dict_set(&opts, "seekable", "0", 0);
    av_dict_set(&opts, "timeout", "5000000", 0);
    av_dict_set(&opts, "hls", "1", 0); // ht modified for hls timeout 500ms; other is 60s
    av_dict_set(&opts, "multiple_requests", "0", 0);
    av_dict_set(&opts, "hls_parse", "0", 0);

    if(strlen(c->header) > 0)
    {
        av_log(NULL, AV_LOG_DEBUG, "open_input(), c->header = %s",c->header);
        av_dict_set(&opts, "headers", c->header, 0);
    }
    // not set user-agent here, set user-agent in ffplayer or use default user-agent in http_connect
    // AppleCoreMedia/1.0.0.9A405 (iPad; U; CPU OS 5_0_1 like Mac OS X; zh_cn)
//    av_dict_set(&opts, "user-agent","Mozilla/4.0 (compatible; MS IE 6.0; (ziva))",0);//"stagefright/1.2 (Linux;Android 4.2.2)"
	if(c->cookies){
		av_dict_set(&opts, "cookies", c->cookies, 0);
	}
#if HLS_DEBUG   
    av_log(NULL, AV_LOG_DEBUG,"%s:url=(%s)",__FUNCTION__,seg->url);                   
#endif

    if (seg->key_type == KEY_NONE) {
        ret = ffurl_open(&var->input, seg->url, AVIO_FLAG_READ,
                          &var->parent->interrupt_callback, &opts);
		//add by xhr, for Bluray
		if(seg->is_seek){
			if(seg->seek_operation_time){
				int64_t filesize = ffurl_size(var->input);

				int64_t totolsize = seg->seek_time + ((seg->seek_time_end - seg->seek_time) / seg->duration) * seg->seek_operation_time;
				int ret = ffurl_seek(var->input,totolsize,SEEK_SET);
				if (ret < 0){
					av_log(NULL, AV_LOG_DEBUG, "Hery, seek err");
				}
			}else{
				int64_t filesize = ffurl_size(var->input);

				int ret = ffurl_seek(var->input,seg->seek_time,SEEK_SET);

				if (ret < 0){
					av_log(NULL, AV_LOG_DEBUG, "Hery, seek err");
				}
			}

		}else{
			if(av_strncasecmp("http://", seg->url, 7)|| av_strncasecmp("https://", seg->url, 8)){
        		if(seg->seek_operation_time){
					int64_t filesize = ffurl_size(var->input);

					int64_t totolsize = (filesize / seg->duration) * seg->seek_operation_time;
					int ret = ffurl_seek(var->input,totolsize,SEEK_SET);
					if (ret < 0){
						av_log(NULL, AV_LOG_DEBUG, "Hery, seek err");
					}

				}
			}

		}
        goto cleanup;
    } else if (seg->key_type == KEY_AES_128) {
        char iv[33], key[33], url[MAX_URL_SIZE];
        if (strcmp(seg->key, var->key_url)) {
            URLContext *uc;
            if (ffurl_open(&uc, seg->key, AVIO_FLAG_READ,
                           &var->parent->interrupt_callback, &opts) == 0) {
                if (ffurl_read_complete(uc, var->key, sizeof(var->key))
                    != sizeof(var->key)) {
                    av_log(NULL, AV_LOG_DEBUG, "Unable to read key file %s\n",
                           seg->key);
                }
                ffurl_close(uc);
            } else {
                av_log(NULL, AV_LOG_DEBUG, "Unable to open key file %s\n",
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

        // not set user-agent here, set user-agent in ffplayer or use default user-agent in http_connect
        //AppleCoreMedia/1.0.0.9A405 (iPad; U; CPU OS 5_0_1 like Mac OS X; zh_cn)
  //      av_dict_set(&opts, "user-agent","Mozilla/4.0 (compatible; MS IE 6.0; (ziva))",0);//"stagefright/1.2 (Linux;Android 4.2.2)"
        av_dict_set(&opts, "timeout", "500000", 0);
        av_dict_set(&opts, "hls", "1", 0); // ht modified for hls timeout 500ms; other is 60s
        if(strlen(c->header) > 0)
        {
            av_dict_set(&opts, "headers", c->header, 0);
        }
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

static int read_seqno(void *opaque)
{
    struct variant *v = opaque;
    HLSContext *c = v->parent->priv_data;
    return c->read_seq_no;
}


static int read_data(void *opaque, uint8_t *buf, int buf_size)
{
    struct variant *v = opaque;
    HLSContext *c = v->parent->priv_data;
    int ret, i,retry = 0,parse_list_retry = 200,read_timeout_cnt = 0;
    int64_t last_load_timeUs = av_gettime();
    if(v->parent->exit_flag){
        return AVERROR_EOF;
    }
restart:
    if (!v->input) {
        /* If this is a live stream and the reload interval has elapsed since
         * the last playlist reload, reload the variant playlists now. */
        int64_t reload_interval = v->n_segments > 0 ?
                                  v->segments[v->n_segments - 1]->duration :
                                  v->target_duration;
        c->seek_flags &= (~AVSEEK_FLAG_READ_NEED_OPEN_NEW_URL);                   
#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG,"reload_interval = %"PRId64"\n",reload_interval);
#endif

        reload_interval *= 1000000;
        int expired_list = 0;
        int sliceSmooth = 0;//Slice smooth handle  by fxw
reload:
        if(ff_check_operate(c->interrupt_callback,OPERATE_SEEK,NULL,NULL))
        {
            av_log(NULL,AV_LOG_ERROR,"read_data, new seek arrival,return immediately");
            return AVERROR(EAGAIN);
        }
        
        if (!v->finished &&
            av_gettime() - v->last_load_time >= reload_interval) {
#if HLS_DEBUG   
    av_log(NULL, AV_LOG_DEBUG,"%s:parse_playlist start:%s",__FUNCTION__,v->url);                   
#endif            
            int64_t startTime = av_gettime();
            ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_START,-1); // c->interrupt_callback
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0){
//                parse_list_retry--;
                if(parse_list_retry < 0){
                    av_log(NULL, AV_LOG_DEBUG,"parse_playlist return ret = %d",ret);
                return ret;
                }
                av_log(NULL, AV_LOG_DEBUG,"read_data():parse_playlist, ret = 0x%x",ret);
				int errorcode = TIMEOUT;
		        if(v != NULL)
	            {
	                URLContext *input = v->input;
	                if(input != NULL)
	                {
			      if(input->errcode != 0)
                              {
	                           errorcode = input->errcode;
	                           av_log(NULL,AV_LOG_DEBUG,"read_data():parse_playlist, errorcode = %d",errorcode);
                              }
	                }
	            }
                ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_ERROR,errorcode);        
                av_usleep(1000*1000);
                if(expired_list == 1){
                    //free_segment_list(v);
                    if (ff_check_interrupt(c->interrupt_callback))
                        return AVERROR_EXIT;
                    av_log(NULL,AV_LOG_DEBUG,"open_input time out, need retry parse list");
                    goto reload;
                }
            }
            else
            {
                 //add info for AliyunOS
                 v->ts_last_time = av_gettime();
               
                 ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_END,(av_gettime()-startTime)/1000);
            }
            /* If we need to reload the playlist again below (if
             * there's still no more segments), switch to a reload
             * interval of half the target duration. */
            reload_interval = v->target_duration * 500000LL;
        }
        else{
            expired_list = 0;
        }
        if (v->cur_seq_no < v->start_seq_no) {
            av_log(NULL, AV_LOG_DEBUG,
                   "skipping %d segments ahead, expired from playlists\n",
                   v->start_seq_no - v->cur_seq_no);
            for(int i = v->cur_seq_no; i < v->start_seq_no; i++)
            {
                ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_START,i);
                ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_ERROR,TIMEOUT);
            }
            if(!v->finished&&c->misHeShiJie==1&&v->n_segments>4){
                v->cur_seq_no = v->start_seq_no + 4;//+ v->n_segments - 1; //reduce jump times
            }else{
                v->cur_seq_no = v->start_seq_no;
            }
        }
		av_log(NULL,AV_LOG_DEBUG,"read_data:cur_seq_no = %d,start_seq_no = %d,n_segments = %d",v->cur_seq_no,
			v->start_seq_no,v->n_segments);
        if (v->cur_seq_no >= v->start_seq_no + v->n_segments) {
            if (v->finished){
                av_log(NULL, AV_LOG_DEBUG, "finished HLS read data AVERROR_EOF\n");
                return AVERROR_EOF;
            }
            while (av_gettime() - v->last_load_time < reload_interval) {
                if (ff_check_interrupt(c->interrupt_callback))
                    return AVERROR_EXIT;
                av_usleep(100*1000);
            }
            /* Enough time has elapsed since the last reload */
            if (ff_check_interrupt(c->interrupt_callback))
            {
                av_log(NULL,AV_LOG_DEBUG,"read_data:ff_check_interrupt");
                return AVERROR_EXIT;
            }
#if HLS_DEBUG   
    av_log(NULL, AV_LOG_DEBUG,"%s:need reload\n",__FUNCTION__);                   
#endif      
            //Slice smooth handle, EXT-X-MEDIA-SEQUENCE  become too smaller than last time suddenly
            if(v->cur_seq_no-(v->start_seq_no + v->n_segments)>v->n_segments*10){
                sliceSmooth++;
                av_log(NULL, AV_LOG_ERROR,"%s:slice smooth times=%d, v->cur_seq_no=%d, v->start_seq_no=%d, v->n_segments=%d\n",
                            __FUNCTION__, sliceSmooth, v->cur_seq_no, v->start_seq_no, v->n_segments);
            }
            if(sliceSmooth>=3){
                v->cur_seq_no = v->start_seq_no;
                av_log(NULL, AV_LOG_ERROR,"%s:slice smooth to seq = %d\n",__FUNCTION__, v->cur_seq_no);
            }else{
                goto reload;
            }
            
        }

        ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_START,v->cur_seq_no);
        v->load_time = av_gettime();
        v->ts_send_time = av_gettime();
        v->ts_first_time = 0;
        v->ts_last_time = 0;
        av_log(NULL, AV_LOG_DEBUG,"%s:need reload,v->cur_seq_no = %d,load_time = %lld",__FUNCTION__,v->cur_seq_no,v->load_time);
        ret = open_input(c , v);
        if (ret < 0){
            av_log(NULL, AV_LOG_DEBUG, "HLS read data %d\n",ret);
			int errorcode = TIMEOUT;
	        if(v != NULL)
            {
                URLContext *input = v->input;
                if(input != NULL)
                {
                      if(input->errcode != 0)
                      {
                          errorcode = input->errcode;
                          av_log(NULL,AV_LOG_DEBUG,"read_data():open_input fail, errorcode = %d",errorcode);
                      }
                }
            }
            ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_ERROR,errorcode);
            
            if(c->abort || v->parent->exit_flag){
                 return AVERROR_EOF;
            }
            if (ff_check_interrupt(c->interrupt_callback))
            {
                av_log(NULL,AV_LOG_DEBUG,"read_data:ff_check_interrupt");
                return AVERROR_EXIT;
            }               
            av_log(NULL, AV_LOG_DEBUG,"read_data:open_input failed,reload");

            int64_t expired_interval = 30;//30s
            expired_interval = (expired_interval<v->target_duration ? v->target_duration : expired_interval)+1;
            expired_interval *= 1000000;                            
            if(!v->finished&&av_gettime() - last_load_timeUs >= expired_interval){
                ffurl_close(v->input);
                v->input = NULL;
                expired_list = 1;
                /*
                av_log(NULL, AV_LOG_DEBUG, "HLS read data skip current seq no %d, open_interval %lld", v->cur_seq_no, open_interval);
                v->cur_seq_no++;
                c->end_of_segment = 1;
                c->cur_seq_no = v->cur_seq_no;
                last_load_timeUs = av_gettime();

                ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_START,i);
                ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_ERROR,TIMEOUT);*/
            }
            av_usleep(200*1000);
            goto reload;
            
        }
        expired_list = 0;
        c->read_seq_no = v->cur_seq_no;
    }
    last_load_timeUs = av_gettime();
	int temp = 1 + (v->n_segments > 0 ?
                                  v->segments[v->cur_seq_no- v->start_seq_no]->duration/2 :
                                  v->target_duration/2);   
    retry = temp;//for huashu auto return by timeout
#if HLS_DEBUG
//    av_log(NULL,AV_LOG_DEBUG,"RETRY = %d",retry);
#endif
	struct segment *seg = v->segments[v->cur_seq_no - v->start_seq_no]; //add by xhr, for Bluray
    int64_t start_time = av_gettime();
    while(retry--){
    ret = ffurl_read(v->input, buf, buf_size);
    if (ret > 0){
        if(0 == v->ts_first_time){
             v->ts_first_time = av_gettime();
        }
		int64_t off_time = av_gettime() - start_time;
		int32_t size = GetSize(c->BandWidthqueue);
		EnQueue(c->BandWidthqueue,ret, off_time);
        
			if(seg->is_seek){
				
				//int64_t pos = avio_tell(&v->pb);
				//int64_t total = avio_size(&v->pb);
				int64_t seek_position = ffurl_seek(v->input, 0, SEEK_CUR);
				//av_log(NULL, AV_LOG_DEBUG, "Hery, seek_position = %lld", seek_position);
				//int64_t total = ffurl_size(v->input);
				//av_log(NULL, AV_LOG_DEBUG, "Hery, pos = %lld, total = %lld", pos, total);
				if (seek_position  >= seg->seek_time_end){
					av_log(NULL, AV_LOG_DEBUG, "Hery, read data break = %lld", seek_position);
					break;
				}
			}
            return ret;
        }
        if(ret == 0 ){
            av_log(NULL,AV_LOG_DEBUG,"read_data len is 0, cur_seq_no=%d", v->cur_seq_no);
            break;
        }
        if(ret == AVERROR_EOF){            
            v->filesize = ffurl_size(v->input);
            v->last_load_time = av_gettime();
            av_log(NULL,AV_LOG_DEBUG, "read_data finished. ");
            break;
        }
        if(c->abort || v->parent->exit_flag){
            av_log(NULL, AV_LOG_DEBUG,"ffurl_read ret = %d",ret);
             return AVERROR_EOF;
        }

	 if(ff_check_operate(c->interrupt_callback,OPERATE_SEEK,NULL,NULL))
	 {
	          av_log(NULL,AV_LOG_ERROR,"read_data, new seek arrival,return immediately");
	     	   return AVERROR(EAGAIN);
	 }
       
        av_log(NULL,AV_LOG_DEBUG,"read:retry=%d,ret=%d",retry,ret);
    }
#if HLS_DEBUG
    av_log(NULL, AV_LOG_DEBUG, "read_data:parse one Segment ret = 0x%x",ret);
#endif
    ffurl_close(v->input);
    v->input = NULL;

    if(ret != -ETIMEDOUT)
    {
        //add info for AliyunOS
        v->ts_last_time = av_gettime();
        
        // send download end event
        int64_t endTime = av_gettime();
        av_log(NULL, AV_LOG_DEBUG, "read_data(), download ts file cur_seq_no = %d,time = %lld,startTime = %lld",v->cur_seq_no,endTime,v->load_time);
        ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_END,(endTime - v->load_time)/1000);
    }
    else
    {
        ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_TIMEOUT, v->cur_seq_no);
        ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_ERROR,TIMEOUT);
    }
    v->cur_seq_no++;

    c->end_of_segment = 1;
    c->cur_seq_no = v->cur_seq_no;
    read_timeout_cnt = 0;
    
    int32_t  estimatebandwidth = 0; 
    if(EstimateBandwidth(c,&estimatebandwidth) == 1)
    {
            int index = c->n_bandwidth_info - 1;
#if HLS_DEBUG        
            av_log(NULL,AV_LOG_DEBUG,"bandwidth estimated at %.2f kbps", estimatebandwidth/ 1024.0f);
#endif        
            // Consider only 80% of the available bandwidth usable.
            while (index > 0 && c->bandwidth_info[index]->bandwidth > estimatebandwidth*8/10)
            {
                --index;
            }
#if HLS_DEBUG
            av_log(NULL,AV_LOG_DEBUG,"bandwidth=%d,c->bandwidth=%d",v->bandwidth,c->bandwidth_info[index]->bandwidth );
#endif
            if(v->bandwidth != c->bandwidth_info[index]->bandwidth)
            {
                av_log(NULL,AV_LOG_DEBUG,"final estimateBH(%d)=%d",index,c->bandwidth_info[index]->bandwidth);
                memcpy(v->url,c->bandwidth_info[index]->url,sizeof(v->url));
                v->bandwidth = c->bandwidth_info[index]->bandwidth;
                v->finished = 0;
#if HLS_DEBUG                
                av_log(NULL,AV_LOG_DEBUG,"there is BandChanged,so recompute loadtime");
#endif
                v->last_load_time = 0;
                
            }
    }
#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG, "read_data:nextQ = %d",c->cur_seq_no);
#endif

    if (v->ctx && v->ctx->nb_streams && v->parent->nb_streams >= v->stream_offset + v->ctx->nb_streams) {
        v->needed = 0;
        for (i = v->stream_offset; i < v->stream_offset + v->ctx->nb_streams;
             i++) {
            if (v->parent->streams[i]->discard < AVDISCARD_ALL)
                v->needed = 1;
        }
    }
    if (!v->needed) {
        av_log(v->parent, AV_LOG_DEBUG, "No longer receiving variant %d\n",
               v->index);
        return AVERROR_EOF;
    }
    goto restart;
}

static int hls_read_header(AVFormatContext *s)
{
	URLContext *u = (s->flags & AVFMT_FLAG_CUSTOM_IO) ? NULL : s->pb->opaque;
    HLSContext *c = s->priv_data;
    int ret = 0, i, j, stream_offset = 0,retry = 0;

	c->cookies = u->cookies;
    c->interrupt_callback = &s->interrupt_callback;
    c->mdynamic = s->mhlsdynamic;
    c->mSort    = s->mhlsSort;
    c->misHeShiJie  = s->mhlsHeShiJie;
    av_log(NULL, AV_LOG_ERROR, "HLS header in:mdynamic=%d,sort=%d,mhlsHeShiJie=%d\n",c->mdynamic,c->mSort,c->misHeShiJie);

    if((s != NULL) && (s->header != NULL))
    {
        memset(c->header,0,MAX_URL_SIZE);
        int length = strlen(s->header);
        if(length < MAX_URL_SIZE)
        {
            memcpy(c->header,s->header,length);
        }
    }

//    ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_START,DOWNLOAD_M3U8_START);
//    int64_t startTime = av_gettime();
loadplaylist:
    if ((ret = parse_playlist(c, s->filename, NULL, s->pb)) < 0){
        if(retry > 5){
                av_log(NULL, AV_LOG_DEBUG, "hls_read_header(), parse_playlist fail,ret = 0x%x",ret);

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

    int64_t endTime = av_gettime();
 //   av_log(NULL, AV_LOG_DEBUG, "hls_read_header(), download m3u8 time = %lld,endTime = %lld,startTime = %lld",(endTime-startTime),endTime,startTime);
 //   ff_send_message(c->interrupt_callback,MEDIA_INFO_DOWNLOAD_END,(endTime-startTime)/1000);
    /* If the playlist only contained variants, parse each individual
     * variant playlist. */
    retry = 0;
loadplaylist1:
    if (c->n_variants > 1 || c->variants[0]->n_segments == 0) {
       // for (i = 0; i < c->n_variants; i++) {
            struct variant *v = c->variants[0];
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
    if(c->n_bandwidth_info> 1)
    {
        SortBandwidth(c);
    }

    if (c->variants[0]->n_segments == 0) {
        av_log(NULL, AV_LOG_WARNING, "Empty playlist\n");
        ret = AVERROR_EOF;
        goto fail;
    }

    /* If this isn't a live stream, calculate the total duration of the
     * stream. */
     av_log(NULL,AV_LOG_DEBUG,"hls_read_header:list finished is %d",c->variants[0]->finished);
    if (c->variants[0]->finished) 
    {
        double duration = 0;
        for (i = 0; i < c->variants[0]->n_segments; i++)
            duration += c->variants[0]->segments[i]->duration;
        s->duration = duration * AV_TIME_BASE;
    }
    else
    {
        if(1 == c->misHeShiJie)
        {
            double duration = 0;
            for (i = 0; i < c->variants[0]->n_segments; i++)
                duration += c->variants[0]->segments[i]->duration;
            s->duration = duration * AV_TIME_BASE;
            av_log(NULL,AV_LOG_DEBUG,"%s: May be He Shi Jie",__FUNCTION__);            

        }
        else
        {
            av_log(NULL,AV_LOG_DEBUG,"%s:livemode now set duration = 20",__FUNCTION__);
            s->duration = 20; // If living stream has no duration,set 20 to avoid random int

        }
    }

    /* Open the demuxer for each variant */
//    for (i = 0; i < c->n_variants; i++) {
        i = 0;
        struct variant *v = c->variants[i];
        AVInputFormat *in_fmt = NULL;
        char bitrate_str[20];
   //     if (v->n_segments == 0)
         //   continue;

        if (!(v->ctx = avformat_alloc_context())) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        v->ctx->ts_debug_flag = s->ts_debug_flag;
		v->ctx->mTaoBaoHevc = 0;
		if(c->mTaobaoHevc == 1)
		{
			v->ctx->mTaoBaoHevc = 1;
		}
		
        v->index  = i;
        v->needed = 1;
        v->parent = s;

        /* If this is a live stream with more than 3 segments, start at the
         * third last segment. */
        v->cur_seq_no = v->start_seq_no;
//        if (!v->finished && v->n_segments > 3)
//            v->cur_seq_no = v->start_seq_no + v->n_segments - 3;

        v->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
        if(c->BandWidthqueue == NULL)
        {
            c->BandWidthqueue = InitQueue();
            av_log(NULL,AV_LOG_DEBUG,"init BandWidthqueue");
        }
		if (strstr(s->filename, "file/m3u8:fd")){
			v->ctx->probesize = 50000000;
		}
        else
        {
            if(s->duration > 20) // discriminate VOD stream 
            {
                v->ctx->probesize = 4*1024*1024;
                v->ctx->max_analyze_duration = 10*1000*1000;
            }
            else    //discriminate living stream
            {
                v->ctx->probesize = 200*1024; // prevent video information err of width or height
                v->ctx->max_analyze_duration = 4*1000*1000;
            }
        }
        ffio_init_context(&v->pb, v->read_buffer, INITIAL_BUFFER_SIZE, 0, v,
                          read_data, NULL, NULL);
        v->pb.read_seqno = read_seqno;
        v->pb.seekable = 0;
        ret = av_probe_input_buffer(&v->pb, &in_fmt, v->segments[0]->url,
                                    NULL, 0, 0);
        if (ret < 0) {
            /* Free the ctx - it isn't initialized properly at this point,
             * so avformat_close_input shouldn't be called. If
             * avformat_open_input fails below, it frees and zeros the
             * context, so it doesn't need any special treatment like this. */
            av_log(s, AV_LOG_DEBUG, "Error when loading first segment '%s'\n", v->segments[0]->url);
            avformat_free_context(v->ctx);
            v->ctx = NULL;
            goto fail;
        }
        v->ctx->pb       = &v->pb;

#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG, "hls:avformat_open_input in \n");
#endif
        ret = avformat_open_input(&v->ctx, v->segments[0]->url, in_fmt, NULL);
#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG, "hls:avformat_open_input out \n");
#endif
        if (ret < 0)
            goto fail;

        v->stream_offset = stream_offset;
        v->ctx->ctx_flags &= ~AVFMTCTX_NOHEADER;
        ret = avformat_find_stream_info(v->ctx, NULL);
        if (ret < 0)
            goto fail;
        snprintf(bitrate_str, sizeof(bitrate_str), "%d", v->bandwidth);
        /* Create new AVStreams for each stream in this variant */
        for (j = 0; j < v->ctx->nb_streams; j++) {
            AVStream *st = avformat_new_stream(s, NULL);
            AVStream *tmp;
            if (!st) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            st->id = i;
            avcodec_copy_context(st->codec, v->ctx->streams[j]->codec);

            tmp = v->ctx->streams[j];
            if(tmp != NULL) 
            {
                av_log(NULL,AV_LOG_ERROR,"%s:source %d:timebse: den %d,num %d",__FUNCTION__,tmp->codec->codec_type,
                    tmp->time_base.den,tmp->time_base.num);

                st->time_base = tmp->time_base;           
            }
    

            if(v->ctx->streams[j]->start_time != AV_NOPTS_VALUE)
            {
                av_log(NULL,AV_LOG_ERROR, "HLS:%d: start_time: %0.3f duration: %0.3f\n", j,
                                    (double) v->ctx->streams[j]->start_time / AV_TIME_BASE,
                                    (double) v->ctx->streams[j]->duration   / AV_TIME_BASE);
                st->start_time  = v->ctx->streams[j]->start_time;
                
            }
            if (v->bandwidth)
                av_dict_set(&st->metadata, "variant_bitrate", bitrate_str,
                                 0);
        }

        stream_offset += v->ctx->nb_streams;
        if(v->ctx->start_time != AV_NOPTS_VALUE)
        {
            av_log(NULL,AV_LOG_ERROR,"HLS: start_time: %0.3f duration: %0.3f bitrate=%d kb/s\n",
                    (double) v->ctx->start_time / AV_TIME_BASE,
                    (double) v->ctx->duration   / AV_TIME_BASE,
                    v->ctx->bit_rate / 1000);
            s->start_time = v->ctx->start_time;

        }

        if (v->ctx->bit_rate > 0 && s->bit_rate <= 0)
        {
            s->bit_rate = v->ctx->bit_rate;
        }
  //  }


    c->first_packet = 1;
    c->first_timestamp = AV_NOPTS_VALUE;
    c->seek_timestamp  = AV_NOPTS_VALUE;
    av_log(NULL,AV_LOG_DEBUG,"HLS header end");
    return 0;
fail:
    free_variant_list(c);
    return ret;
}

static int recheck_discard_flags(AVFormatContext *s, int first)
{
    HLSContext *c = s->priv_data;
    int i, changed = 0;

    /* Check if any new streams are needed */
    for (i = 0; i < c->n_variants; i++)
        c->variants[i]->cur_needed = 0;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        struct variant *var = c->variants[s->streams[i]->id];
        if (st->discard < AVDISCARD_ALL)
            var->cur_needed = 1;
    }
    for (i = 0; i < c->n_variants; i++) {
        struct variant *v = c->variants[i];
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

static int hls_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    HLSContext *c = s->priv_data;
    int ret, i, minvariant = -1;

    if (c->first_packet) {
        recheck_discard_flags(s, 1);
        c->first_packet = 0;
    }

start:
    c->end_of_segment = 0;
   // for (i = 0; i < c->n_variants; i++) {
        i = 0;
        struct variant *var = c->variants[i];
        /* Make sure we've got one buffered packet from each open variant
         * stream */
        if (var->needed && !var->pkt.data) {
            while (1) {
                int64_t ts_diff;
                AVStream *st;
                ret = av_read_frame(var->ctx, &var->pkt);
                //var->pkt.seq = var->cur_seq_no;
                if (ret < 0) {
#if HLS_DEBUG
                    av_log(NULL, AV_LOG_DEBUG, "hls_read_packet = %d\n",ret);
#endif
                    if(ff_check_operate(c->interrupt_callback,OPERATE_SEEK,NULL,NULL))
                    {
                        av_log(NULL,AV_LOG_ERROR,"hls_read_packet, new seek arrival,return immediately");
                        return AVERROR(EAGAIN);
                    }
                    
                    if (!url_feof(&var->pb) && ret != AVERROR_EOF){
                        return ret;
                    }
#if HLS_DEBUG
                    av_log(NULL, AV_LOG_DEBUG, "hls_read_packet AVERROR_EOF ret = %d\n",ret);
#endif
                    reset_packet(&var->pkt);
                    break;
                } else {
                    if (c->first_timestamp == AV_NOPTS_VALUE)
                        c->first_timestamp = var->pkt.dts;
                }

                if (c->seek_timestamp == AV_NOPTS_VALUE && !(c->seek_flags & AVSEEK_FLAG_READ_NEED_OPEN_NEW_URL))
                    break;

                if (var->pkt.dts == AV_NOPTS_VALUE) {
                    c->seek_timestamp = AV_NOPTS_VALUE;
                    break;
                }

                st = var->ctx->streams[var->pkt.stream_index];
                ts_diff = av_rescale_rnd(var->pkt.dts, AV_TIME_BASE,
                                         st->time_base.den, AV_ROUND_DOWN) -
                          c->seek_timestamp;
                
                if (st->codec && (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)) {
                    if (c->seek_flags & AVSEEK_FLAG_READ_NEED_OPEN_NEW_URL) {
                        av_log(NULL, AV_LOG_DEBUG, "read packet not open url yet, drop it...\n");
                        av_free_packet(&var->pkt);
//                        reset_packet(&var->pkt);
                        continue;
                    }
                }
                
                if ((c->seek_flags  & AVSEEK_FLAG_ANY ||
                                     var->pkt.flags & AV_PKT_FLAG_KEY)) {
                    c->seek_timestamp = AV_NOPTS_VALUE;
                    break;
                }
            }
        }
        /* Check if this stream has the packet with the lowest dts */
        if (var->pkt.data) {
            if(minvariant < 0) {
                minvariant = i;
            } else {
                struct variant *minvar = c->variants[minvariant];
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
        *pkt = c->variants[minvariant]->pkt;
        pkt->stream_index += c->variants[minvariant]->stream_offset;
        reset_packet(&c->variants[minvariant]->pkt);
        return 0;
    }

    av_log(NULL, AV_LOG_DEBUG, "HLS read AVERROR_EOF\n");
    return AVERROR_EOF;
}

static int hls_close(AVFormatContext *s)
{
	av_log(NULL, AV_LOG_DEBUG, "Hery, hls_close");
    HLSContext *c = s->priv_data;

    AVIOParams *params = c->interrupt_callback->ioparams;
    if(params) params->willclose = 1;
    
    free_variant_list(c);
    av_log(NULL,AV_LOG_DEBUG,"hls_close:DestroyQueue");
    if(c->BandWidthqueue != NULL)
    {
        DestroyQueue(c->BandWidthqueue);
        c->BandWidthqueue = NULL;
    }
    if(params){
        if(params->hd){
            ffurl_closep(&params->hd);
        }
       av_free(params);
       c->interrupt_callback->ioparams = NULL;
    }
    return 0;
}

static int hls_seek_new_url(AVFormatContext *s)
{
    HLSContext *c = s->priv_data;
    av_log(NULL,AV_LOG_DEBUG,"%s",__FUNCTION__);
    int ret = 0;
    int retry = 0;
    int i = 0;
    int j = 0;
    int stream_offset = 0;
    char* new_url = NULL;

     // get the seek url
    ff_check_operate(c->interrupt_callback,OPERATE_GET_URL,NULL,(void**)&new_url);
    if(new_url == NULL)
    {
        return -1;
    }

    AVIOParams *params = c->interrupt_callback->ioparams;
    if(params&&params->type==1&&params->redirect==1&&params->url[0]!='\0')
    {
        params->redirect = 0;//avoid parse_playlist use redirect url
    }
    
    // release all variant in HlsContext
    if(c != NULL)
    {
        free_variant_list(c);
    }

    // release band queue
    if(c->BandWidthqueue != NULL)
    {
        DestroyQueue(c->BandWidthqueue);
        c->BandWidthqueue = NULL;
    }

    URLContext *u = (s->flags & AVFMT_FLAG_CUSTOM_IO) ? NULL : s->pb->opaque;
    if(u != NULL)
    {
        c->cookies = u->cookies;
    }
    c->interrupt_callback = &s->interrupt_callback;
    c->mdynamic = s->mhlsdynamic;
    c->mSort    = s->mhlsSort;
    c->misHeShiJie  = s->mhlsHeShiJie;

    ret = parse_playlist(c, new_url, NULL, NULL);
    if (c->n_variants == 0) 
    {
        av_log(NULL, AV_LOG_ERROR, "Empty playlist\n");
        ret = AVERROR_EOF;
        goto fail;
    }

loadplaylist1:
    if (c->n_variants > 1 || c->variants[0]->n_segments == 0)
    {
        struct variant *v = c->variants[0];
        if ((ret = parse_playlist(c, v->url, v, NULL)) < 0)
        {
            if(retry > 5)
            {
                goto fail;
            }
            else
            {
                if(ret == AVERROR_EXIT || s->exit_flag)
                {
                    ret = AVERROR_EOF;
                    goto fail;
                }
                retry++;
                av_usleep(100*1000);
                goto loadplaylist1;
            }
        }
    }

    //the next code is coying form hls_read_head
    if(c->n_bandwidth_info> 1)
    {
        SortBandwidth(c);
    }

    if (c->variants[0]->n_segments == 0)
    {
        av_log(NULL, AV_LOG_WARNING, "Empty playlist\n");
        ret = AVERROR_EOF;
        goto fail;
    }

    /* If this isn't a live stream, calculate the total duration of the
     * stream. */
    av_log(NULL,AV_LOG_DEBUG,"hls_seek_new_url:list finished is %d",c->variants[0]->finished);
    if (c->variants[0]->finished) 
    {
        double duration = 0;
        for (i = 0; i < c->variants[0]->n_segments; i++)
            duration += c->variants[0]->segments[i]->duration;
        s->duration = duration * AV_TIME_BASE;
    }
    else
    {
        if(1 == c->misHeShiJie)
        {
            double duration = 0;
            for (i = 0; i < c->variants[0]->n_segments; i++)
                duration += c->variants[0]->segments[i]->duration;
            s->duration = duration * AV_TIME_BASE;
            av_log(NULL,AV_LOG_DEBUG,"%s: May be He Shi Jie",__FUNCTION__);            
        }
        else
        {
            av_log(NULL,AV_LOG_DEBUG,"%s:livemode now set duration = 20",__FUNCTION__);
            s->duration = 20; // If living stream has no duration,set 20 to avoid random int

        }
    }

    /* Open the demuxer for each variant */
    i = 0;
    struct variant *v = c->variants[i];
    AVInputFormat *in_fmt = NULL;
    char bitrate_str[20];

    if (!(v->ctx = avformat_alloc_context())) 
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    v->ctx->ts_debug_flag = s->ts_debug_flag;
    v->ctx->mTaoBaoHevc = 0;
    if(c->mTaobaoHevc == 1)
    {
        v->ctx->mTaoBaoHevc = 1;
    }
	
    v->index  = i;
    v->needed = 1;
    v->parent = s;

    /* If this is a live stream with more than 3 segments, start at the
     * third last segment. */
    v->cur_seq_no = v->start_seq_no;

    v->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
    if(c->BandWidthqueue == NULL)
    {
        c->BandWidthqueue = InitQueue();
        av_log(NULL,AV_LOG_DEBUG,"init BandWidthqueue");
    }
    
    if (strstr(s->filename, "file/m3u8:fd"))
    {
        v->ctx->probesize = 50000000;
    }
    else
    {
        if(s->duration > 20) // discriminate VOD stream 
        {
            v->ctx->probesize = 4*1024*1024;
            v->ctx->max_analyze_duration = 10*1000*1000;
        }
        else    //discriminate living stream
        {
            v->ctx->probesize = 200*1024; // prevent video information err of width or height
            v->ctx->max_analyze_duration = 4*1000*1000;
        }
    }
    ffio_init_context(&v->pb, v->read_buffer, INITIAL_BUFFER_SIZE, 0, v,
                      read_data, NULL, NULL);
    v->pb.read_seqno = read_seqno;
    v->pb.seekable = 0;
    ret = av_probe_input_buffer(&v->pb, &in_fmt, v->segments[0]->url,
                                NULL, 0, 0);
    if (ret < 0) {
        /* Free the ctx - it isn't initialized properly at this point,
         * so avformat_close_input shouldn't be called. If
         * avformat_open_input fails below, it frees and zeros the
         * context, so it doesn't need any special treatment like this. */
        av_log(s, AV_LOG_DEBUG, "Error when loading first segment '%s'\n", v->segments[0]->url);
        avformat_free_context(v->ctx);
        v->ctx = NULL;
        goto fail;
    }
    v->ctx->pb       = &v->pb;

    ret = avformat_open_input(&v->ctx, v->segments[0]->url, in_fmt, NULL);

    if (ret < 0)
        goto fail;

    v->stream_offset = stream_offset;
    v->ctx->ctx_flags &= ~AVFMTCTX_NOHEADER;
    ret = avformat_find_stream_info(v->ctx, NULL);
    if (ret < 0)
        goto fail;
    
    snprintf(bitrate_str, sizeof(bitrate_str), "%d", v->bandwidth);
    /* Create new AVStreams for each stream in this variant */
    for (j = 0; j < v->ctx->nb_streams; j++) 
    {
        AVStream *st = avformat_new_stream(s, NULL);
        AVStream *tmp;
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        st->id = i;
        avcodec_copy_context(st->codec, v->ctx->streams[j]->codec);

        tmp = v->ctx->streams[j];
        if(tmp != NULL) 
        {
            av_log(NULL,AV_LOG_ERROR,"%s:source %d:timebse: den %d,num %d",__FUNCTION__,tmp->codec->codec_type,
            tmp->time_base.den,tmp->time_base.num);
            st->time_base = tmp->time_base;           
        }


        if(v->ctx->streams[j]->start_time != AV_NOPTS_VALUE)
        {
            av_log(NULL,AV_LOG_ERROR, "HLS:%d: start_time: %0.3f duration: %0.3f\n", j,
                        (double) v->ctx->streams[j]->start_time / AV_TIME_BASE,
                        (double) v->ctx->streams[j]->duration   / AV_TIME_BASE);
            st->start_time  = v->ctx->streams[j]->start_time;
        }
        if (v->bandwidth)
            av_dict_set(&st->metadata, "variant_bitrate", bitrate_str,0);
    }

    stream_offset += v->ctx->nb_streams;
    if(v->ctx->start_time != AV_NOPTS_VALUE)
    {
        av_log(NULL,AV_LOG_ERROR,"HLS: start_time: %0.3f duration: %0.3f bitrate=%d kb/s\n",
                (double) v->ctx->start_time / AV_TIME_BASE,
                (double) v->ctx->duration   / AV_TIME_BASE,
                v->ctx->bit_rate / 1000);
        s->start_time = v->ctx->start_time;

    }
    
    c->first_packet = 1;
    c->first_timestamp = AV_NOPTS_VALUE;
    c->seek_timestamp  = AV_NOPTS_VALUE;
    av_log(NULL,AV_LOG_DEBUG,"hls_seek_new_url ok");

    return 0;
fail:
    free_variant_list(c);
    return -1;
}
static int hls_read_seek(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    HLSContext *c = s->priv_data;
    int i, j, ret;
#if HLS_DEBUG
        av_log(NULL,AV_LOG_DEBUG,"%s:start:timestamp=%lld",__FUNCTION__,timestamp);
#endif

    //add by hh for wase seek using new url
    if(flags & AVSEEK_FLAG_SEEK_USE_NEW_URL)
    {
        int result = hls_seek_new_url(s);
        if(result < 0)
        {
            av_log(NULL,AV_LOG_ERROR,"hls_read_seek retult = %d",result);
            return AVERROR_SEEK_FAIL;
        }

        return 0;
    }

    /*
    *  (金亚问题)CNTV 直播流无法seek的问题，听从ht建议，增加对n_bandwidth_info的判读(直播流一般没有动态码率)
    */
    if ((flags & AVSEEK_FLAG_BYTE) || (!c->variants[0]->finished && c->n_bandwidth_info > 1))
    {
#if HLS_DEBUG
    av_log(NULL,AV_LOG_DEBUG,"%s:ENOSYS",__FUNCTION__);
#endif
        return AVERROR(ENOSYS);
    }

    c->seek_flags     |= (flags | AVSEEK_FLAG_READ_NEED_OPEN_NEW_URL);
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

    ret = AVERROR(EIO);
   // for (i = 0; i < c->n_variants; i++) {
        /* Reset reading */
        i = 0;
        struct variant *var = c->variants[i];
        double pos = c->first_timestamp == AV_NOPTS_VALUE ? 0 :
                      av_rescale_rnd(c->first_timestamp, 1, stream_index >= 0 ?
                               s->streams[stream_index]->time_base.den :
                               AV_TIME_BASE, flags & AVSEEK_FLAG_BACKWARD ?
                               AV_ROUND_DOWN : AV_ROUND_UP);
        double dispos = -1;
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
	    av_log(NULL,AV_LOG_DEBUG,"hls_read_seek:pos%f",pos);

        av_log(NULL,AV_LOG_DEBUG,"c->seek_flags = %d",c->seek_flags);
        if(var)
        {
            if(c->seek_flags & AVSEEK_FLAG_SPECIAL_URL)
            {
                av_log(NULL,AV_LOG_DEBUG,"yst or BestTv lookback,need changed pos");
                pos = 0;
                c->seek_flags &= (~AVSEEK_FLAG_SPECIAL_URL);
            }
        }        
        /* Locate the segment that contains the target timestamp */
        av_log(NULL,AV_LOG_DEBUG,"var->n_segments = %d,timestamp = %lld,pos = %f",var->n_segments,timestamp,pos);

		//edit by xhr ,  Unified, timeus is equal to mSeekTimeUs,not to add s->start_time
		//Coordination to ffmpeg hls to use
		timestamp += pos;

		
		for (j = 0; j < var->n_segments; j++) {
            if(var->segments[j]->discontinuity){
                dispos = pos;
            }

            /*
            * 如果seek时间落在切片的前半段，则使用该切片
            */
            if (timestamp >= pos &&
                timestamp <= pos + var->segments[j]->duration/2) {
                var->cur_seq_no = var->start_seq_no + j;
//				var->segments[j]->seek_operation_time = timestamp - pos;      //add by xhr, for Bluray seek operation
                ret = 0;
                break;
            }
            
            /*
            * 如果seek时间落在切片的后半段，则使用该切片的下一个切片
            */
            if(timestamp > pos + var->segments[j]->duration/2 && timestamp < pos + var->segments[j]->duration)
            {
                if(j < (var->n_segments - 1))
                {
                    j++;
                }
                var->cur_seq_no = var->start_seq_no + j;
                ret = 0;
                break;
            }
            pos += var->segments[j]->duration;
        }
		av_log(NULL,AV_LOG_DEBUG,"j1 = %d",j);
        //如果timestamp=0,期望从头开始播放，但是小于pos，初始值,那么需要修正cur_seq_no
        if(j == var->n_segments && timestamp== 0 && timestamp < pos)
        {
            j = 0;
            var->cur_seq_no = var->start_seq_no + j;
        }
        if(j == var->n_segments){
              var->cur_seq_no = var->start_seq_no + j;
              ret = 0;
        }

        av_log(NULL,AV_LOG_DEBUG,"j = %d",j);
        if (ret)
            c->seek_timestamp = AV_NOPTS_VALUE;
    if(dispos > 0){
#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG, "c->seek_timestamp= %lld,dispos = %lf\n",c->seek_timestamp,dispos*1000000);
#endif
		c->seek_timestamp -= ((int64_t)dispos)*1000000;
#if HLS_DEBUG
        av_log(NULL, AV_LOG_DEBUG, "seek_timestamp after = %lld \n",c->seek_timestamp);
#endif
	}else{
        c->seek_timestamp = AV_NOPTS_VALUE;
    }
    av_log(NULL,AV_LOG_DEBUG,"hls_seek:tm=%lld,curQ=%d",timestamp,var->cur_seq_no);
    return ret;
}

static int hls_probe(AVProbeData *p)
{
    /* Require #EXTM3U at the start, and either one of the ones below
     * somewhere for a proper match. */
#if HLS_DEBUG
    av_log(NULL,AV_LOG_DEBUG,"hls_probe in\n");
#endif
    if (strncmp(p->buf, "#EXTM3U", 7))
        return 0;
    if (strstr(p->buf, "#EXT-X-STREAM-INF:")     ||
        strstr(p->buf, "#EXT-X-TARGETDURATION:") ||
        strstr(p->buf, "#EXT-X-MEDIA-SEQUENCE:"))
        return AVPROBE_SCORE_MAX;
    return 0;
}

AVInputFormat ff_hls_demuxer = {
    .name           = "hls,applehttp",
    .long_name      = NULL_IF_CONFIG_SMALL("Apple HTTP Live Streaming"),
    .priv_data_size = sizeof(HLSContext),
    .read_probe     = hls_probe,
    .read_header    = hls_read_header,
    .read_packet    = hls_read_packet,
    .read_close     = hls_close,
    .read_seek      = hls_read_seek,
};
