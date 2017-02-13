#include "ig.h"

#include "dsputil.h"
#include "bytestream.h"
#include "libavutil/colorspace.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"

#define RGBA(r,g,b,a) (((unsigned)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define POSITIVE(x) ( x > 0 ? x : 0)

#define YUV_TO_RGB0(y,cb,cr)\
{\
	r  = POSITIVE(floor(1.164*y + 1.793*cr - 247.628));\
	g = POSITIVE(floor(1.164*y -0.213*cb - 0.533*cr + 77.364));\
	b = POSITIVE(floor(1.164*y +2.112*cb - 288.46));\
}

enum SegmentType {
    PALETTE_SEGMENT      = 0x14,
    PICTURE_SEGMENT      = 0x15,
    ICS_SEGMENT          = 0x18,
    DISPLAY_SEGMENT      = 0x80,
};

static void deleteIG(IG* ig)
{
    int i = 0;
    int j = 0;
    int k = 0;
    if(ig != NULL)
    {
        if(ig->mPictures != NULL)
        {
            for (i=0;i < ig->mPicturesCount;i++) 
            {
                if(ig->mPictures[i].rle != NULL)
                {
                    av_freep(&ig->mPictures[i].rle);
                }

                if(ig->mPictures[i].picture != NULL)
                {
                    av_freep(&ig->mPictures[i].picture);
                }
            }
            av_freep(&ig->mPictures);
        }

        if(ig->mPalettes != NULL)
        {
            av_freep(&ig->mPalettes);
        }
            
        ig->mPicturesCount = 0;

        if(ig->mICS != NULL)
        {
            ICS *ics = ig->mICS;
            if(ics->mPages != NULL)
            {
                for (i=0;i<ics->mPages_count;i++) 
                {
                    IGSPage *page = &ics->mPages[i];
                    if(page != NULL)
                    {
                        for (j=0;j<page->bogs_count;j++) 
                        {
                            IGSBOG *bog = &page->bogs[j];
                            if(bog != NULL)
                            {
                                for (k=0;k<bog->buttons_count;k++) 
                                {
                                    IGSButton *button = &bog->buttons[k];
                                    if(button != NULL)
                                    {
                                        if (button->cmds != NULL) 
                                        {
                                            av_freep(&button->cmds);
                                        }
                                        av_freep(&bog->buttons);
                                    }
                                }
                            }
                        }

                        av_freep(&page->bogs);
                    }
                    EffectSequence* effect = page->mInEffectSequnce;
                    if(effect != NULL)
                    {
                        av_freep(&effect->mWindow);
                        av_freep(&effect->mEffect);
                        av_freep(&page->mInEffectSequnce);
                    }
                    effect = page->mOutEffectSequnce;
                    if(effect != NULL)
                    {
                        av_freep(&effect->mWindow);
                        av_freep(&effect->mEffect);
                        av_freep(&page->mOutEffectSequnce);
                    }
                }
                av_freep(&ics->mPages);
            }
            av_freep(&ig->mICS);
        }
    }
}


static av_cold int init_decoder(AVCodecContext *avctx)
{
    avctx->pix_fmt         = AV_PIX_FMT_PAL8;
    IGContext* context = (IGContext*)avctx->priv_data;
    int i = 0;
    if(context != NULL)
    {
        context->mIg = NULL;
        context->mICSBuffer = NULL;
        context->mICSDataSize = 0;
        context->mICSDataTotalSize = 0;
    }
    av_log(NULL, AV_LOG_ERROR,"Bluray IG:init_decoder()");
    return 0;
}

static av_cold int close_decoder(AVCodecContext *avctx)
{
    int i,j,k;
    av_log(NULL, AV_LOG_ERROR,"Bluray IG:close_decoder()");
    
    IGContext* context = (IGContext*)avctx->priv_data;
    if(context != NULL)
    {
        if(context->mICSBuffer != NULL)
        {
            av_freep(&context->mICSBuffer);
        }
        context->mICSDataSize = 0;
        context->mICSDataTotalSize = 0;
        
        if(context->mIg != NULL)
        {
            av_log(NULL, AV_LOG_ERROR,"close_decoder(),free IG");
            deleteIG(context->mIg);
            av_freep(&context->mIg);
        }
        
        av_freep(&avctx->priv_data);
    }

    return 0;
}


static void initIG(IG* ig)
{
    if(ig != NULL)
    {
        ig->mPicturesCount = 0;
        ig->mPalettesCount = 0;
        ig->mICS = NULL;
        ig->mPalettes = NULL;
        ig->mPictures = NULL;
    }
}


static void initPage(IGSPage* page)
{
    if(page != NULL)
    {
        page->def_button = -1;
        page->sel_button = -1;
        page->in_time = 0;
        page->timeout = 0;
        page->palette = 0;

        page->bogs = NULL;
        page->bogs_count = 0;

        page->id = 0;

        page->mInEffectSequnce = NULL;
        page->mOutEffectSequnce = NULL;
    }
}

static void initBog(IGSBOG* bog)
{
    if(bog != NULL)
    {
        bog->buttons = NULL;
        bog->buttons_count = 0;
        bog->cur_button = -1;
        bog->def_button = -1;
    }
}

static void initButton(IGSButton* button)
{
    if(button != NULL)
    {
        button->x = 0;
        button->y = 0;
        button->cmds = NULL; 
        button->cmds_count = 0;
        button->autoAction = 0;
        button->pRepeat[0] = 0;
        button->pRepeat[1] = 0;
        button->pRepeat[2] = 0;
        button->mAnimtaionId = 0;
        button->mSelectedSoundId = 0xff;
        button->mActiveSoundId = 0xff;
    }
}

static int displayWindow(AVCodecContext *avctx,char** p,EffectSequence *effectSequence)
{
    if(p == NULL)
        return -1;
    
    unsigned char windowId = bytestream_get_byte(&(*p));
    unsigned short windowX = bytestream_get_be16(&(*p));
    unsigned short windowY = bytestream_get_be16(&(*p));
    unsigned short windowWidth = bytestream_get_be16(&(*p));
    unsigned short windowHeight = bytestream_get_be16(&(*p));

    
//    av_log(avctx, AV_LOG_ERROR, "display_window():id:%d, x:%d, y:%d, width:%d, height:%d\n",windowId,windowX,windowY,windowWidth,windowHeight);
#if 0
    if(effectSequence != NULL)
    {
        effectSequence->mWindow
    }
#endif
    return 0;
}

static int displayCompositionObject(AVCodecContext *avctx,char** p)
{
    if(p == NULL) 
        return -1;

    unsigned short objId = bytestream_get_be16(&(*p)); 
    unsigned char window = bytestream_get_byte(&(*p)); 
    unsigned char force = bytestream_get_byte(&(*p));
    unsigned char x = bytestream_get_be16(&(*p)); 
    unsigned char y = bytestream_get_be16(&(*p));
//	LOGD("display_composition_object():obj_id:%d, window:%d, force:%s, pos(%d,%d)", p[0]<<8|p[1], p[2], ((p[3]&0x40)?"yes":"no"), p[4]<<8|p[5], p[6]<<8|p[7]);
    if (force&0x80)	//cropping?
	{
//		LOGD("display_composition_object():crop{h_pos:%d,v_pos:%d,w:%d,h:%d}", p[8]<<8|p[9], p[10]<<8|p[11], p[12]<<8|p[13], p[14]<<8|p[15]);
        unsigned short cropX = bytestream_get_be16(&(*p)); 
        unsigned short cropY = bytestream_get_be16(&(*p)); 
        unsigned short cropWidth = bytestream_get_be16(&(*p)); 
        unsigned short cropHeight = bytestream_get_be16(&(*p)); 
	}

    return 0;
}


static int displayEffect(AVCodecContext *avctx,char** p)
{
    if(p == NULL)
        return -1;

    int effectDuration = bytestream_get_be24(&(*p));
    unsigned char paletteRefId = bytestream_get_byte(&(*p));
	int obj_num = bytestream_get_byte(&(*p));

//  LOGD("display_composition_object():effect_duration:%d, palette_id:%d,number_of_composition_obj:%d\n", p[0]<<16|p[1]<<8|p[2], p[3], obj_num);

//    av_log(avctx, AV_LOG_ERROR, "displayEffect(), obj_num = %d",obj_num);
    if (obj_num > MAX_EFFECT_OBJECT_NUM)
    {
        av_log(avctx, AV_LOG_ERROR, "displayEffect(), obj_num = %d,error",obj_num);
//        return -1;
    }
	for (int i = 0; i < obj_num; ++i)
	{
		displayCompositionObject(avctx,p);
	}

    return 0;
}


static int displayEffectSequence(AVCodecContext *avctx,char** p,EffectSequence *effectSequence)
{
    if(p == NULL)
        return -1;

    if(effectSequence != NULL)
    {
        effectSequence->mWindow = NULL;
        effectSequence->mEffect = NULL;
    }
    
	int win_num = bytestream_get_byte(&(*p));
//	av_log(avctx, AV_LOG_ERROR, "displayEffectSequence(), window Number =%d",win_num);
    if(effectSequence != NULL)
    {
        effectSequence->mNumberWidow = win_num;
    }
    
    if(win_num > MAX_WINDOW_NUM)
    {
        av_log(avctx, AV_LOG_ERROR, "displayEffectSequence(), window Number =%d,error",win_num);
//        return -1;
    }

//    LOGD("displayEffectSequence(), win_num = %d",win_num);
	for (int i = 0; i < win_num; ++i)
	{
		if(displayWindow(avctx,p,effectSequence) != 0)
		{
 //           return -1;
		}
	}

	int effect_num = bytestream_get_byte(&(*p));
//    av_log(avctx, AV_LOG_ERROR, "displayEffectSequence(), effect_num =%d",effect_num);
    if(effect_num > MAX_EFFECT_NUM)
    {
        av_log(avctx, AV_LOG_ERROR, "displayEffectSequence(), effect Number =%d,error",effect_num);
//        return -1;
    }
    
	for (int i = 0; i < effect_num; ++i)
	{
        displayEffect(avctx,p);
	//	if(displayEffect(p) != 0)
         // return -1;
	}

    return 0;
}



/**
 * Parse the picture segment packet.
 *
 * The picture segment contains details on the sequence id,
 * width, height and Run Length Encoded (RLE) bitmap data.
 *
 * @param avctx contains the current codec context
 * @param buf pointer to the packet to process
 * @param buf_size size of packet to process
 * @todo TODO: Enable support for RLE data over multiple packets
 */
 static int parsePictureSegment(AVCodecContext *avctx,
                                  const uint8_t *buf, int buf_size)
{
    IGContext *  ctx = avctx->priv_data;
    if((ctx == NULL) || (ctx->mIg == NULL))
    {
        av_log(avctx, AV_LOG_ERROR, "parsePictureSegment(), IGContext == NULL");
        return NULL;
    }

    IG* ig = ctx->mIg;
    IGSPicture *pic = NULL;
    uint16_t oid;
    uint8_t version,sequence_desc;
    unsigned int rle_bitmap_len, width, height;
    
    if (buf_size <= 4)
        return -1;

    /* skip 3 unknown bytes: Object ID (2 bytes), Version Number */
    oid = bytestream_get_be16(&buf);
    buf_size -= 2;

    //version,just skip
    version = bytestream_get_byte(&buf);
    buf_size -= 1;

    /* Read the Sequence Description to determine if start of RLE data or appended to previous RLE */
    sequence_desc = bytestream_get_byte(&buf);
    buf_size -= 1;
    uint8_t firstSequence = (sequence_desc & 0x80) >> 7;
    uint8_t lastSequence = (sequence_desc & 0x40) >> 6;

 //   av_log(avctx, AV_LOG_ERROR, "parsePictureSegment(): firstSequence = %d,lastSequence = %d",firstSequence,lastSequence);
    if(firstSequence == 1)
    {
        if (buf_size <= 7)
            return -1;
        
        /* Decode rle bitmap length, stored size includes width/height data */
        rle_bitmap_len = bytestream_get_be24(&buf) - 2*2;
        buf_size -= 3;

        /* Get bitmap dimensions from data */
        width  = bytestream_get_be16(&buf);
        buf_size -= 2;
        height = bytestream_get_be16(&buf);
        buf_size -= 2;
        
        /* Make sure the bitmap is not too large */ 
        if(width > 1920 || height > 1080)
        {
            av_log(avctx, AV_LOG_ERROR, "Bitmap dimensions (%dx%d) larger than video.\n", width, height);
            return -1;
        }
        
        /* add this picture */
//        av_log(avctx, AV_LOG_ERROR, "parsePictureSegment() mPicturesCount = %d",ig->mPicturesCount);
        ig->mPicturesCount++;
        pic = av_realloc(ig->mPictures, ig->mPicturesCount * sizeof(IGSPicture));
        if (pic == NULL) {
            av_log(avctx, AV_LOG_ERROR, "Can't reallocate bitmaps table.\n");
            ig->mPicturesCount--;
            return -2;
        }
        ig->mPictures = pic;
        pic = &(ig->mPictures[ig->mPicturesCount-1]);

        pic->id = oid;
        pic->w  = width;
        pic->h  = height;
        pic->picture = NULL;
//        av_log(avctx, AV_LOG_ERROR, "parsePictureSegment() id = %d,width = %d,height = %d",pic->id,width,height);
        pic->rle = av_malloc(rle_bitmap_len);
        if (pic->rle == NULL)
        {
            av_log(avctx, AV_LOG_ERROR, "Can't malloc rle space, id = %d,width = %d,height = %d",pic->id,pic->w,pic->h);
            return -1;
        }
        
        memset(pic->rle,0,rle_bitmap_len);
        pic->rle_buffer_size=rle_bitmap_len;

        if (buf_size>pic->rle_buffer_size) 
            buf_size=pic->rle_buffer_size;
        
        memcpy(pic->rle, buf, buf_size);
        pic->rle_data_len = buf_size;
        pic->rle_remaining_len = rle_bitmap_len - buf_size;
    }
    else
    {
        /* Additional RLE data to picture */
        /* first find the picture accord the id */
        for(int position = ig->mPicturesCount-1; position >= 0; position --)
        {
            pic = &ig->mPictures[position];
            if((pic != NULL) && (pic->id == oid))
            {
                if (buf_size > pic->rle_remaining_len)
                {
                    av_log(avctx, AV_LOG_ERROR, "parsePictureSegment(): too much data in Pictures[%d], error",position);
                    return -1;
                }
                if(pic->rle != NULL)
                {
                    memcpy(pic->rle + pic->rle_data_len, buf, buf_size);
                    pic->rle_data_len += buf_size;
                    pic->rle_remaining_len -= buf_size;
                }

                return 0;
            }
        }
    }

    return 0;
}
 
/**
* Parse the palette segment packet.
*
* The palette segment contains details of the palette,
* a maximum of 256 colors can be defined.
*
* @param avctx contains the current codec context
* @param buf pointer to the packet to process
* @param buf_size size of packet to process
*/
static int parsePaletteSegment(AVCodecContext *avctx,const uint8_t *buf, int buf_size)
{
    IGContext *ctx = avctx->priv_data;
    if((ctx == NULL) || (ctx->mIg == NULL))
    {
        av_log(avctx, AV_LOG_ERROR, "parsePaletteSegment(): IGContext == NULL");
        return -1;
    }

    IG* ig = ctx->mIg;

    const uint8_t *buf_end = buf + buf_size;
    const uint8_t *cm      = ff_cropTbl + MAX_NEG_CROP;
    int color_id;
    int y, cb, cr, alpha;
    int r, g, b, r_add, g_add, b_add;
    
    /* add this picture */
    ig->mPalettesCount++;
    IGSPalette * pal = av_realloc(ig->mPalettes, ig->mPalettesCount * sizeof(IGSPalette));
    if (pal == NULL) {
        av_log(avctx, AV_LOG_ERROR, "parsePaletteSegment():Can't reallocate palettes table.\n");
        ig->mPalettesCount--;
        return -1;
    }
    
    ig->mPalettes = pal;
    pal = &(ig->mPalettes[ig->mPalettesCount-1]);
    memset(&pal->clut,0,sizeof(pal->clut));
    
//    av_log(avctx, AV_LOG_ERROR, "parsePaletteSegment(): mPalettesCount = %d,mPalettes = 0x%x",ig->mPalettesCount,ig->mPalettes);
    /* Skip two null bytes */
    pal->id = bytestream_get_byte(&buf);
    int version = bytestream_get_byte(&buf);

/*    av_log(avctx, AV_LOG_ERROR, "New palette %04X @ %p: idx=%d, size=%d, %d colors.\n",
           oid, pal, ctx->mPalettesCount-1, buf_size-2, (buf_size-2)/5);*/
    while (buf < buf_end) {
        color_id  = bytestream_get_byte(&buf);
        y         = bytestream_get_byte(&buf);
        cr        = bytestream_get_byte(&buf);
        cb        = bytestream_get_byte(&buf);
        alpha     = bytestream_get_byte(&buf);

        YUV_TO_RGB0(y, cb, cr);
        
        if(r > 255) r = 255;
        if(g > 255) g = 255;
        if(b > 255) b = 255;

 //       av_log(avctx, AV_LOG_DEBUG, "  Color %d := (%d,%d,%d,%d)\n", color_id, r, g, b, alpha);

        double temp = ((double)alpha)/255;
        r *= temp;
        g *= temp;
        b *= temp;
        alpha *= temp;

        /* Store color in palette */
        pal->clut[color_id] = RGBA(r,g,b,alpha);
    }  
    return 0;
}

static void freeBuffer(AVCodecContext *avctx)
{
    if(avctx != NULL)
    {
        IGContext* cxt = (IGContext*)avctx->priv_data;
        if(cxt != NULL)
        {
            if(cxt->mICSBuffer != NULL)
            {
                av_log(avctx, AV_LOG_ERROR, "freeBuffer()");
                av_freep(&cxt->mICSBuffer);
            }

            cxt->mICSDataSize = 0;
            cxt->mICSDataTotalSize = 0;
        }
    }
}

/**
 * Parse the button segment packet.
 *
 * The button segment contains details on the interactive graphics.
 *
 * @param avctx contains the current codec context
 * @param buf pointer to the packet to process
 * @param buf_size size of packet to process
 * @todo TODO: Implement cropping
 */
static int parseICS(AVCodecContext *avctx,const uint8_t *buf, int buf_size)
{
    IGContext *ctx = avctx->priv_data;
    const uint8_t *start = buf;
    uint8_t* endBuffer = buf+buf_size;
    int i, page_cnt;
    uint32_t to;

    if(ctx->mIg == NULL)
    {
        av_log(avctx, AV_LOG_ERROR, "parseICS():Can't malloc mICS \n");
        return -1;
    }

    if(ctx->mIg->mICS == NULL)
    {
        ctx->mIg->mICS = av_malloc(sizeof(ICS));
        if(ctx->mIg->mICS  == NULL)
        {
            av_log(avctx, AV_LOG_ERROR, "parseICS():Can't malloc mICS \n");
            return -1;
        }
    }
    
    ICS* ic = ctx->mIg->mICS;
    ic->mWidth = bytestream_get_be16(&buf);
    ic->mHeight = bytestream_get_be16(&buf);

//    av_log(avctx, AV_LOG_ERROR, "Interactive Graphics dimensions %dx%d\n", ic->mWidth, ic->mHeight);
//    if (av_image_check_size(ic->mWidth, ic->mHeight, 0, avctx) >= 0)
//        avcodec_set_dimensions(avctx, ic->mWidth, ic->mHeight);
    
    // skip framerate (0x20 => 2 => 24fps, 0x60 => 50fps)
    ic->mFrameRate = ((bytestream_get_byte(&buf)& 0xF0) >> 4);
    buf += 2; // composition_number (0x0000) 8.9
    buf += 1; // composition state (0x80) 10
//    av_log(avctx, AV_LOG_ERROR, "ic->mFrameRate = %d",ic->mFrameRate);
    unsigned char sequence = bytestream_get_byte(&buf);//sequence descriptor 11
    unsigned char first = (sequence & 0x80)>>7; // start flag
    unsigned char last = (sequence & 0x40)>>6; // end flag
//    av_log(avctx, AV_LOG_ERROR, "sequence = %d,first = %d,last = %d",sequence,first,last);
    uint8_t*  data = NULL;
    //  first squence
    if(first == 1)
    {
        int ics_len =  buf[0]<<16|buf[1]<<8|buf[2];//load the composition length 12,13,14
        buf += 3;

        // end flag
        if(last == 1)
        {
            data = buf;
        }
        else
        {
            // mallo space to store this data
         //   freeBuffer(avctx);
            if(ctx->mICSBuffer != NULL)
            {
                av_freep(&ctx->mICSBuffer);
            }
            ctx->mICSDataSize = 0;
            ctx->mICSDataTotalSize = ics_len;
            ctx->mICSBuffer = av_malloc(ics_len);
            if(ctx->mICSBuffer == NULL)
            {
                av_log(avctx, AV_LOG_DEBUG, "parseICS(), malloc mICSTempBuffer fail");
                return -1;
            }
            
            memcpy(&ctx->mICSBuffer[ctx->mICSDataSize],buf,endBuffer- buf);
            ctx->mICSDataSize += endBuffer - buf;
            return 0;
        }
    }
    else
    {
        int size = endBuffer- buf;
        if((size + ctx->mICSDataSize) <= ctx->mICSDataTotalSize)
        {
            if(ctx->mICSBuffer != NULL)
            {
                memcpy(&ctx->mICSBuffer[ctx->mICSDataSize],buf,size);
                ctx->mICSDataSize += size;
            }
            else
            {
                av_log(avctx, AV_LOG_ERROR, "parseICS(), mICSTempBuffer = NULL");
                return -1;
            }
        }
        else
        {
            av_log(avctx, AV_LOG_ERROR, "parseICS(), wrong length");
            freeBuffer(avctx);
            return -1;
        }

        // if it's not the last package,then return ,else parse
        if(last == 0)
        {
            return 0;
        }
        
        data = ctx->mICSBuffer;
    }

    if(data == NULL)
    {
        freeBuffer(avctx);
        return -1;
    }
    
    // ten more byte skipped (output generated by TMpegEnc Authoring 5)
    ic->mUiMode = bytestream_get_byte(&data);
    if((ic->mUiMode&0x80) == 0)   /* load the UI and stream model */
    {
         /*get the upper 32 bits of the 33 here 45khz granularity */
        ic->compositionTimeoutPTS = (bytestream_get_byte(&data) & 0x01) << 31; 
        ic->compositionTimeoutPTS |= bytestream_get_be32(&data) >> 1; 

        ic->selectionTimeoutPTS = (bytestream_get_byte(&data) & 0x01) << 31; 
        ic->selectionTimeoutPTS |= bytestream_get_be32(&data) >> 1;

        int time = ic->selectionTimeoutPTS/45000;

        av_log(avctx, AV_LOG_ERROR,"parseICS(): selectionTimeoutPTS = %lld,time = %d",ic->selectionTimeoutPTS,time);
    }
    
//    buf+=4; // skip IN_time (0xc0000163) 
    ic->userTimeout = bytestream_get_be24(&data);/* load user timeout duration */
    ic->mPages_count = bytestream_get_byte(&data);
    ic->mPages = av_malloc(ic->mPages_count*sizeof(IGSPage));
//    av_log(avctx, AV_LOG_ERROR, "parseICS(), mPages_count = %d",ic->mPages_count);
    if(ic->mPages == NULL)
    {
        av_log(avctx, AV_LOG_ERROR, "parseICS(): malloc Pages fail");
        freeBuffer(avctx);
        return -1;
    }

    /* pages loop */
    for (i=0;i<ic->mPages_count;i++) 
    {
        IGSPage *page = &(ic->mPages[i]);
        int j, bog_cnt;

        initPage(page);
        /* skip page_id */
        page->id = bytestream_get_byte(&data);
//        av_log(avctx, AV_LOG_ERROR, "parseICS(), page->id = %d",page->id);
        /* skip version */
        int version = bytestream_get_byte(&data);

        /* skip UO MASK Table*/
        data += 8;
        
        // in effect
        page->mInEffectSequnce = av_malloc(sizeof(EffectSequence));
        displayEffectSequence(avctx,&data,page->mInEffectSequnce);

        // out effect
        page->mOutEffectSequnce = av_malloc(sizeof(EffectSequence));
        displayEffectSequence(avctx,&data,page->mOutEffectSequnce);
        
/*        if(displayEffectSequence(buf) != 0) // in effect
        {
            LOGD("parseICS():displayEffectSequence error");
            return -1;
        }
   		if(displayEffectSequence(buf) != 0) // out effect
   		{
            LOGD("parseICS():displayEffectSequence error");
   		    return -1;
   		}*/
   		
        page->frameRate = bytestream_get_byte(&data);/* framerate */
 //       av_log(avctx, AV_LOG_ERROR, "parseICS(),page->frameRate = %d",page->frameRate);
        page->def_button = bytestream_get_be16(&data);// default select button
        page->sel_button = page->def_button;

        int act_btn = bytestream_get_be16(&data);/* default activated button*/
        page->palette = bytestream_get_byte(&data); // palette id

        bog_cnt = bytestream_get_byte(&data);

 //       av_log(avctx, AV_LOG_ERROR, "parseICS(): BOG count = %d,default selected button id = %d,default active button id = %d",bog_cnt,page->def_button, act_btn);

        page->bogs = av_malloc(bog_cnt*sizeof(IGSBOG));//new IGSBOG[bog_cnt];
        if (page->bogs != NULL)
        {
            page->bogs_count = bog_cnt;
        }
        else 
        {
            av_log(avctx, AV_LOG_ERROR, "parseICS(): malloc bogs fail");
            break;
        }
        
        /* bogs loop */
//        av_log(avctx, AV_LOG_ERROR, "parseICS(): page->bogs_count = %d",page->bogs_count);
        for (j=0;j<page->bogs_count;j++) 
        {
            IGSBOG *bog = &(page->bogs[j]);
            
            int k, button_cnt;

            initBog(bog);
            bog->def_button = bytestream_get_be16(&data); //default valid button id
            bog->cur_button = bog->def_button;
//            av_log(avctx, AV_LOG_ERROR, "parseICS():BOG id = %d",j);
            button_cnt = bytestream_get_byte(&data); // button count
            
//            av_log(avctx, AV_LOG_ERROR, "parseICS():BOG id = %d, button_cnt = %d,bog->def_button = %d",j,button_cnt,bog->def_button);

            bog->buttons = av_malloc(button_cnt*sizeof(IGSButton));//new IGSButton[button_cnt];
            if (bog->buttons != NULL)  
            {
                bog->buttons_count = button_cnt;
            }
            else 
            {
                bog->buttons_count = 0;
                av_log(avctx, AV_LOG_ERROR, "parseICS(): malloc IGSButton fail");
                break;
            }
            
            /* buttons loop */
            for (k=0;k<bog->buttons_count;k++) 
            {
                IGSButton *but = &(bog->buttons[k]);
                
                initButton(but);
                
                but->id        = bytestream_get_be16(&data);
                but->numericValue  = bytestream_get_be16(&data); //Numeric Select Value
                but->autoAction = (bytestream_get_byte(&data) & 0x80) >> 7; //auto action flag
                but->x         = bytestream_get_be16(&data);
                but->y         = bytestream_get_be16(&data);
                // neighbor info
                but->n[0]      = bytestream_get_be16(&data); // up button
                but->n[1]      = bytestream_get_be16(&data); // low button
                but->n[2]      = bytestream_get_be16(&data); // left button
                but->n[3]      = bytestream_get_be16(&data); // right button
//                av_log(avctx, AV_LOG_ERROR, "parseICS(): but->id = %d",but->id);
//                av_log(avctx, AV_LOG_ERROR,"parseICS(): button->id = %d : x=%d, y=%d,auto = %d", but->id, but->x, but->y,but->autoAction);
//                av_log(avctx, AV_LOG_ERROR,"up = %d low = %d, left = %d, right = %d", but->n[0], but->n[1], but->n[2], but->n[3]);
                // normal state info (animation)
                but->pstart[0] = bytestream_get_be16(&data);  // normal start object Id
                but->pstop[0]  = bytestream_get_be16(&data);  // normal stop object Id
                but->pRepeat[0] = ((bytestream_get_byte(&data) & 0x80)>>7);// repeat flag
                
                // select state info (animation)
                but->mSelectedSoundId = bytestream_get_byte(&data);// selected sound id
                but->pstart[1] = bytestream_get_be16(&data);
                but->pstop[1]  = bytestream_get_be16(&data);
                but->pRepeat[1] = ((bytestream_get_byte(&data) & 0x80)>>7);// repeat flag

                // activie
                but->mActiveSoundId = bytestream_get_byte(&data);// sound
                but->pstart[2]  = bytestream_get_be16(&data);
                but->pstop[2]   = bytestream_get_be16(&data);
                but->cmds_count = bytestream_get_be16(&data);

//                av_log(avctx, AV_LOG_ERROR,"parseICS(): id = %d,start normal = %d,select = %d,activie = %d,",but->id,but->pstart[0],but->pstart[1],but->pstart[2]);
//                av_log(avctx, AV_LOG_ERROR,"parseICS(): id = %d,stop normal = %d,select = %d,activie = %d,",but->id,but->pstop[0],but->pstop[1],but->pstop[2]);

                but->cmds = NULL;
                if (but->cmds_count) 
                {
                    but->cmds = av_malloc(but->cmds_count*sizeof(IGCommand));//new NavigationCommand[but->cmds_count];
//                    av_log(avctx, AV_LOG_ERROR, "parseICS(): but->id = %d,cmds_count = %d",but->id,but->cmds_count);
                    if(but->cmds != NULL)
                    {
                        for(int cmdCount = 0; cmdCount < but->cmds_count; cmdCount++)
                        {
                            but->cmds[cmdCount].mOpCode = bytestream_get_be32(&data);
                            but->cmds[cmdCount].mDstOperand = bytestream_get_be32(&data);
                            but->cmds[cmdCount].mSrcOperand = bytestream_get_be32(&data);
                        }
                    }
                    else
                    {
                        av_log(avctx, AV_LOG_ERROR, "parseICS(): malloc but->cmds fail");
                    }
                }
                
                /* load extra flags */
                but->mState = BUTTON_DISABLED;
        
      //          LOGD("pics=%04X/%04X/%04X, cmds=%d\n", but->pstart[0], but->pstart[1], but->pstart[2], but->cmds_count);
                
                but->mAnimtaionId = but->pstart[0];

                // set this button current if not already set
                if (bog->cur_button == 0xffff) bog->cur_button = but->id;
            }

        }

        // set selected button if not already set
        if (page->sel_button == 0xffff) {
            for (j=0;j<page->bogs_count;j++) {
                IGSBOG *bog = &(page->bogs[j]);
                int k;
                for (k=0;k<bog->buttons_count;k++) {
                    IGSButton *but = &(bog->buttons[k]);
                    // search first button with nav to other button
                    if (but->n[0] != but->id ||
                        but->n[1] != but->id ||
                        but->n[2] != but->id ||
                        but->n[3] != but->id) {
                        page->sel_button = but->id;
                        break;
                    }
                }
                if (page->sel_button != 0xffff) break;
            }
        }
    }

    freeBuffer(avctx);
    return 0;
}



static int decode(AVCodecContext *avctx, uint8_t *data, int *data_size,
                  AVPacket *avpkt)
{
    IG    *ctx = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    AVSubtitle *sub    = data;

    const uint8_t *buf_end;
    uint8_t       segment_type;
    int           segment_length;
    int i;

 //   av_log(avctx, AV_LOG_ERROR, "blu-ray decode IGS packet");

    *data_size = 0;

    /* Ensure that we have received at a least a segment code and segment length */
    if (buf_size < 3)
        return -1;
    
    buf_end = buf + buf_size;

    /* Step through buffer to identify segments */
    while (buf < buf_end) {
        segment_type   = bytestream_get_byte(&buf);
        segment_length = bytestream_get_be16(&buf);

//        av_dlog(avctx, "Segment Length %d, Segment Type %x\n", segment_length, segment_type);

        switch (segment_type) {
        case PALETTE_SEGMENT:
            parsePaletteSegment(avctx, buf, segment_length);
            break;

        case PICTURE_SEGMENT: // Bitmap image data compressed with an RLE compression schema
            parsePictureSegment(avctx,buf, segment_length);
            break;
            
        case ICS_SEGMENT:
            parseICS(avctx,buf,segment_length);
            break;

        case DISPLAY_SEGMENT:
            break;
            
        default:
            av_log(avctx, AV_LOG_ERROR, "Unknown IGS segment type 0x%x, length %d\n",
                   segment_type, segment_length);
            break;
        }

        buf += segment_length;
    }

    return buf_size;
}



static const AVOption options[] = {
    { NULL },
};

static const AVClass ig_class = {
    .class_name = "IG decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_blurayig_decoder = {
    .name           = "BlurayIG",
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_BLURAY_IG,
    .priv_data_size = sizeof(IGContext), 
    .init           = init_decoder,
    .close          = close_decoder,
    .decode         = decode,
    .long_name      = NULL_IF_CONFIG_SMALL("blu-ray ig decoder"),
    .priv_class     = &ig_class,
};


