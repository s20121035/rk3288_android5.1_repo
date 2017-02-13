/*
 * Blu-ray Text subtitle decoder
 * Copyright (c) 2014 Fuzhou Rockchip Electronics Co., Ltd
 *
 * This file is using to parse Blu-ray's Text Subtitle
 * Author:hh@rock-chips.com
 */
#include "bluraytextsubtitle.h"
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

static void deleteTextSubtitleDialog(DialogPresentation* dialog)
{
    av_log(NULL, AV_LOG_ERROR,"deleteTextSubtitleDialog()");
    if(dialog != NULL)
    {
        if(dialog->mPalette != NULL)
        {
            av_log(NULL, AV_LOG_ERROR,"av_free(&dialog->mPalette)");
            av_freep(&dialog->mPalette);
        }

        if(dialog->mRegion != NULL)
        {
            av_log(NULL, AV_LOG_ERROR,"dialog->mRegion");
            for(int i = 0; i < dialog->mRegionCount; i++)
            {
                for(int j = 0; j < dialog->mRegion[i].mLineCount; j++)
                {
                    TextLineData* lineData = dialog->mRegion[i].mLineData[j];
                    if(lineData != NULL)
                    {
                        if(lineData->mText != NULL)
                        {
                            av_freep(&lineData->mText);
                        }

                        if(lineData->mInLineStyleValues != NULL)
                        {
                            av_freep(&lineData->mInLineStyleValues);
                        }

                        if(lineData->mInLineStyleType != NULL)
                        {
                            av_freep(&lineData->mInLineStyleType);
                        }

                        if(lineData->mInLineStylePosition != NULL)
                        {
                            av_freep(&lineData->mInLineStylePosition);
                        }
                    }
                }

                dialog->mRegion[i].mLineCount = 0;
            }
            dialog->mRegionCount = 0;
            av_log(NULL, AV_LOG_ERROR,"dialog->mRegion");
            av_freep(&dialog->mRegion);
        }

        av_freep(&dialog);
    }
}

static void deleteDialogStyle(DialogStyle* style)
{
    if(style != NULL)
    {
        av_log(NULL, AV_LOG_ERROR,"deleteDialogStyle(),av_freep(&style->mRegionStyle)");
        if(style->mRegionStyle != NULL)
        {
            av_freep(&style->mRegionStyle);
        }
        av_log(NULL, AV_LOG_ERROR,"deleteDialogStyle(),av_freep(&style->mUserStyle)");
        if(style->mUserStyle != NULL)
        {
            av_freep(&style->mUserStyle);
        }
        av_log(NULL, AV_LOG_ERROR,"deleteDialogStyle(),av_freep(&style->mPalette)");
        if(style->mPalette != NULL)
        {
            av_freep(&style->mPalette);
        }
        av_log(NULL, AV_LOG_ERROR,"deleteDialogStyle(),av_freep(&style)");
        av_freep(&style);
    }
}

static av_cold int init_decoder(AVCodecContext *avctx)
{
    avctx->pix_fmt     = AV_PIX_FMT_PAL8;
    TextSubtitle *text = avctx->priv_data;
    if(text != NULL)
    {
        text->mDialog = NULL;
        text->mDialogStyle = NULL;
    }
    av_log(NULL, AV_LOG_ERROR,"Bluray Text Subtitle:init_decoder()");
    return 0;
}

static av_cold int close_decoder(AVCodecContext *avctx)
{
    av_log(NULL, AV_LOG_ERROR,"Bluray Text Subtitle:close_decoder()");
    TextSubtitle *text = avctx->priv_data;
    if(text != NULL)
    {
        deleteTextSubtitleDialog(text->mDialog);

        deleteDialogStyle(text->mDialogStyle);
  //      av_freep(&text->mDialogStyle);
    }
    av_freep(&avctx->priv_data);
    return 0;
}

static int decodeFontSytle(uint8_t** buffer,FontStyle* fontStyle)
{
    if(fontStyle == NULL)
    {
        av_log(NULL, AV_LOG_ERROR,"decodeFontSytle(), param is invalid");
        return -1;
    }

    unsigned char font_style = bytestream_get_byte(buffer);//getByte(buffer); buffer ++;
    fontStyle->mBold = font_style & 1;
    fontStyle->mItalic = (font_style & 2)>>1;
    fontStyle->mOutLineBorder = (font_style & 4)>>2;

    av_log(NULL, AV_LOG_ERROR,"decodeFontSytle(),bold = %d,italic = %d,mOutLineBorder = %d",fontStyle->mBold,fontStyle->mItalic,fontStyle->mOutLineBorder);

    return 0;
}


static int decodeRect(uint8_t** buf,TextSubtitleRect* rect)
{
    if((buf == NULL) || (rect == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeRect(), param is invalid");
        return -1;
    }

    rect->x = (int)bytestream_get_be16(&(*buf)); 
    rect->y = (int)bytestream_get_be16(&(*buf)); 
    rect->mWidth = (int)bytestream_get_be16(&(*buf)); 
    rect->mHight = (int)bytestream_get_be16(&(*buf));

    av_log(NULL, AV_LOG_ERROR,"decodeRect(), x = %d,y = %d,width = %d,height = %d",rect->x,rect->y,rect->mWidth,rect->mHight);
    
    return 0;
}


static int decodeRegionInfo(uint8_t** buf,RectRegion* region)
{
    if((buf == NULL) || (region == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeRegionInfo(), param is invalid");
        return -1;
    }

    decodeRect(buf, &region->mRegion);
    region->mBackColorPaletteRefId = bytestream_get_byte(&(*buf));
 //   (*buf)+= 1;

    //skip 8bits
    bytestream_get_byte(&(*buf));
 //   (*buf)+= 1;

    return 0;
}

static int decodeRegionStyle(uint8_t** buf,RegionStyle* style)
{
    if((buf == NULL) ||(style == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeRegionStyle(), param is invalid");
        return -1;
    }

    style->mRegionStyleId = bytestream_get_byte(&(*buf));

    av_log(NULL, AV_LOG_ERROR,"decodeRegionStyle(), mRegionStyleId = %d",style->mRegionStyleId);
 //   LOGD("decodeRegionStyle()******,mRegionStyleId = %d",style->mRegionStyleId);
    decodeRegionInfo(buf, &style->mRegion);
    decodeRect(buf, &style->mTextRect);

    /* Set additional text info for the region style */
    style->mTextFlow = bytestream_get_byte(&(*buf));
    style->mTextHalign = bytestream_get_byte(&(*buf));
    style->mTextValign = bytestream_get_byte(&(*buf)); 
    style->mLineSpace = bytestream_get_byte(&(*buf));

    /* Set font style info for the region style */
    style->mFontIdRef = bytestream_get_byte(&(*buf)); 
    decodeFontSytle(buf, &style->mFontStyle);

    style->mFontSize = bytestream_get_byte(&(*buf));
    style->mFontColor = bytestream_get_byte(&(*buf)); 
    style->mOutLineColor = bytestream_get_byte(&(*buf)); 
    style->mOuntLIneThickness = bytestream_get_byte(&(*buf)); 

    av_log(NULL, AV_LOG_ERROR,"decodeRegionStyle(), font size = %d,font color = %d",style->mFontSize,style->mFontColor);
    av_log(NULL, AV_LOG_ERROR,"decodeRegionStyle(), mOutLineColor = %d,mOuntLIneThickness = %d",style->mOutLineColor,style->mOuntLIneThickness);
    
    
    return 0;
}

static int decodeUserStyle(uint8_t** buf,UserStyle* style)
{
    if((buf == NULL) || (style == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeUserStyle(),param is invalid");
        return -1;
    }

    #if 0
    TextSTInfo->pDSS->region_style[i].user_control_style[j].user_style_id = TextST_SPB[ulSPBOffset];
    ulSPBOffset++;

    /* Set region info for the user control style */
    TextSTInfo->pDSS->region_style[i].user_control_style[j].region_horizontal_position_diretion  = TextST_SPB[ulSPBOffset] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].region_horizontal_position_delta     = MAKE_WORD(&TextST_SPB[ulSPBOffset]) & 0x7fff;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].region_vertical_position_direction   = TextST_SPB[ulSPBOffset + 2] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].region_vertical_position_delta       = MAKE_WORD(&TextST_SPB[ulSPBOffset + 2]) & 0x7fff;
    ulSPBOffset += 4;

    /* Set font info for the user control style */
    TextSTInfo->pDSS->region_style[i].user_control_style[j].font_size_inc_dec    = TextST_SPB[ulSPBOffset] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].font_size_delta      = TextST_SPB[ulSPBOffset] & 0x7f;
    ulSPBOffset++;

    /* Set text box info for the user control style */
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_horizontal_position_direction   = TextST_SPB[ulSPBOffset] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_horizontal_position_delta       = MAKE_WORD(&TextST_SPB[ulSPBOffset]) & 0x7fff;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_vertical_position_direction     = TextST_SPB[ulSPBOffset + 2] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_vertical_position_delta         = MAKE_WORD(&TextST_SPB[ulSPBOffset + 2]) & 0x7fff;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_width_inc_dec                   = TextST_SPB[ulSPBOffset + 4] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_width_delta                     = MAKE_WORD(&TextST_SPB[ulSPBOffset + 4]) & 0x7fff;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_height_inc_dec                  = TextST_SPB[ulSPBOffset + 6] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].text_box_height_delta                    = MAKE_WORD(&TextST_SPB[ulSPBOffset + 6]) & 0x7fff;
    ulSPBOffset += 8;

    /* Set line space info for the user control style */
    TextSTInfo->pDSS->region_style[i].user_control_style[j].line_space_inc_dec   = TextST_SPB[ulSPBOffset] >> 7;
    TextSTInfo->pDSS->region_style[i].user_control_style[j].line_space_delta     = TextST_SPB[ulSPBOffset] & 0x7f;
    ulSPBOffset++;
    #endif
    

    style->mUserStyleId = bytestream_get_byte(&(*buf));

    /* Set region info for the user control style */
    unsigned short temp = (unsigned short)bytestream_get_be16(&(*buf)); 
    style->mRegionHorizontalDiretion = (unsigned char)(temp >> 15);
    style->mRegionHorizontaDelta = (short)(temp & 0x7fff);
    
    temp = (unsigned short)bytestream_get_be16(&(*buf));
    style->mRegionVerticalDirection = (unsigned char)(temp >> 15);
    style->mRegionVerticalDelta = (short)(temp & 0x7fff);

    /* Set font info for the user control style */
    unsigned char Font = bytestream_get_byte(&(*buf));
    style->mFontIncDec = Font>>7;
    style->mFontSizeDelta = Font&0x7f;

    /* Set text box info for the user control style */
    temp = (unsigned short)bytestream_get_be16(&(*buf));
    style->mTextBoxHorizontalDirection = (unsigned char)(temp >> 15);
    style->mTextBoxHorizontalDelta = (short)(temp & 0x7fff);

    temp = (unsigned short)bytestream_get_be16(&(*buf));
    style->mTextBoxVerticalDirection = (unsigned char)(temp >> 15);
    style->mTextBoxVerticalDelta = (short)(temp & 0x7fff);
    
    temp = (unsigned short)bytestream_get_be16(&(*buf)); 
    style->mTextBoxWidthIncDec = (unsigned char)(temp >> 15);
    style->mTextBoxWidthDelta = (short)(temp & 0x7fff);

    temp = (unsigned short)bytestream_get_be16(&(*buf));
    style->mTextBoxHeightIncDec = (unsigned char)(temp >> 15);
    style->mTextBoxHeightDelta = (short)(temp & 0x7fff);

    /* Set line space info for the user control style */
    unsigned char Line = bytestream_get_byte(&(*buf));
    style->mLineSpaceIncDec = Line>>7;
    style->mLineSpaceDelta = Line&0x7f;

    av_log(NULL, AV_LOG_ERROR,"decodeUserStyle(),mUserStyleId = %d",style->mUserStyleId);
    return 0;
}

static int decodePalette(uint8_t** buf,int count,PaletteEntry* palette)
{
    if((buf != NULL) && palette != NULL)
    {
        int color_id;
        int y, cb, cr, alpha;
        int r, g, b, r_add, g_add, b_add;
    
        for(int i = 0; i < count; i++)
        {
            color_id  = bytestream_get_byte(&(*buf));
            y         = bytestream_get_byte(&(*buf)); 
            cr        = bytestream_get_byte(&(*buf));
            cb        = bytestream_get_byte(&(*buf));
            alpha     = bytestream_get_byte(&(*buf));

            YUV_TO_RGB0(y, cb, cr);
 #if 1           
            if(r > 255) r = 255;
            if(g > 255) g = 255;
            if(b > 255) b = 255;

     //       av_log(avctx, AV_LOG_DEBUG, "  Color %d := (%d,%d,%d,%d)\n", color_id, r, g, b, alpha);

            double temp = ((double)alpha)/255;
            r *= temp;
            g *= temp;
            b *= temp;
            alpha *= temp;
#endif
            /* Store color in palette */
            palette->color[color_id] = RGBA(r,g,b,alpha);
//            av_log(NULL, AV_LOG_ERROR, "color_id = %d,r = %d,g = %d,b = %d,alpha = %d",color_id,r,g,b,alpha);
        }
        
        return 0;
    }

    return -1;
}


static int decodeDialogStyle(uint8_t** buf,DialogStyle* style)
{
    if((buf == NULL) || (style == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeDialogStyle(), param is invalid");
        return -1;
    }

    av_log(NULL, AV_LOG_ERROR,"decodeDialogStyle()");
    unsigned char temp = bytestream_get_byte(&(*buf));
    style->mPlayerStyleFlag = temp>>7;
//    LOGD("decodeDialogStyle(), buffer = 0x%x",buffer);

    //skip 8bits
    bytestream_get_byte(&(*buf));

    style->mRegoinStyleCount = bytestream_get_byte(&(*buf));
    style->mUserStyleCount = bytestream_get_byte(&(*buf)); 
    av_log(NULL, AV_LOG_ERROR,"mRegoinStyleCount = %d,mUserStyleCount = %d",style->mRegoinStyleCount,style->mUserStyleCount);
    style->mRegionStyle = NULL;
    if(style->mRegoinStyleCount > 0)
    {
        style->mRegionStyle = av_malloc(sizeof(RegionStyle)*style->mRegoinStyleCount);
        if(style->mRegionStyle != NULL)
        {
            memset(style->mRegionStyle,0,sizeof(RegionStyle)*style->mRegoinStyleCount);
            for(int i = 0; i < style->mRegoinStyleCount; i++)
            {
                decodeRegionStyle(buf,&style->mRegionStyle[i]);
            }
        }
    }

    style->mUserStyle = NULL;
    if(style->mUserStyleCount > 0)
    {
        style->mUserStyle = av_malloc(sizeof(UserStyle)*style->mUserStyleCount);//new UserStyle[style->mUserStyleCount];
        if(style->mUserStyle != NULL)
        {
            memset(style->mUserStyle,0,sizeof(UserStyle)*style->mUserStyleCount);
            for(int i = 0; i < style->mUserStyleCount; i++)
            {
                decodeUserStyle(buf,&style->mUserStyle[i]);
            }
        }
    }

    int length = bytestream_get_be16(&(*buf));//bytestream_get_be16(buf); (*buf)+= 2;
    int count = length/5;
    
    style->mPalette = av_malloc(sizeof(PaletteEntry));//new PaletteEntry();
    if(style->mPalette != NULL)
    {
        memset(style->mPalette,0,sizeof(PaletteEntry));
        decodePalette(buf,count,style->mPalette);
    }

    return 0;
}


static int parseDialogStyle(AVCodecContext *avctx,uint8_t *data,uint8_t* buffer,int length)
{
    if((avctx == NULL) || (buffer == NULL) || (length <= 0) || (data == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"parseDialogStyle(), param is invalid");
        return 0;
    }

    AVSubtitle *subtitle = (AVSubtitle *)data;
    subtitle->dialogStyle = av_malloc(sizeof(DialogStyle));
    if(subtitle->dialogStyle != NULL)
    {
        if(decodeDialogStyle(&buffer,subtitle->dialogStyle) != 0) 
        {
            deleteDialogStyle(subtitle->dialogStyle);
            av_log(NULL, AV_LOG_ERROR,"parseDialogStyle(), decodeDialogStyle fail");
            return 0;
        }
    }

    int totalDialog = bytestream_get_be16(&buffer);
    av_log(NULL, AV_LOG_ERROR,"parseDialogStyle():mTotalDialog = %d",totalDialog);

    if (totalDialog < 1) 
    {
        av_log(NULL, AV_LOG_ERROR,"parseDialogStyle():no dialog segments");
    }

    return 1;
}

static void initLineData(TextLineData* line)
{
    if(line != NULL)
    {
        line->mText = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
        line->mInLineStyleValues = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
        line->mInLineStyleType = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
        line->mInLineStylePosition = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
        if(line->mText != NULL)
        {
            memset(line->mText, 0, MAX_CHARS_PER_DPS_REGION);
        }

        if(line->mInLineStyleValues)
        {
            memset(line->mInLineStyleValues, 0, MAX_CHARS_PER_DPS_REGION);
        }

        if(line->mInLineStyleType)
        {
            memset(line->mInLineStyleType, 0, MAX_CHARS_PER_DPS_REGION);
        }
        
        if(line->mInLineStylePosition != NULL)
        {
            memset(line->mInLineStylePosition, 0xff, MAX_CHARS_PER_DPS_REGION);
        }
    }
}

static int decodeDialogRegion(uint8_t** buf,DialogRegion* region)
{
    if(region == NULL)
    {
        av_log(NULL, AV_LOG_ERROR,"decodeDialogRegion(): param is invalid");
        return -1;
    }

    unsigned char temp = bytestream_get_byte(&(*buf));
    region->mContinousPresentFlag = temp>>7;
    ////// maybe some problem here ////////////////

    
    region->mForceOnFlag = (temp>>6)&0x01;
    region->mRegionStyleRefId = bytestream_get_byte(&(*buf)); 
    
    int length = bytestream_get_be16(&(*buf)); 
    int byteRead = 0;

    /* set initial counter values */
    int textStringOffset      = 0;
    int inlineStyleOffset     = 0;
    int numberOfInlineStyles  = 0;

    int numberOfLine = 0;
    region->mLineCount = 1;
    TextLineData* lineData = av_malloc(sizeof(TextLineData));//new TextLineData();
    if(lineData == NULL)
    {
        av_log(NULL, AV_LOG_ERROR,"decodeDialogRegion(): malloc lineData fail");
        return -1;
    }
#if 0
    lineData->mText = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
    lineData->mInLineStyleValues = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
    lineData->mInLineStyleType = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
    lineData->mInLineStylePosition = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
    if(lineData->mText != NULL)
    {
        memset(lineData->mText, 0, MAX_CHARS_PER_DPS_REGION);
    }
    if(lineData->mInLineStylePosition != NULL)
    {
        memset(lineData->mInLineStylePosition, 0xff, MAX_CHARS_PER_DPS_REGION);
    }
#endif
    initLineData(lineData);
    while (byteRead < length) 
    {
        /* parse header */
        unsigned char code = bytestream_get_byte(&(*buf)); 
        byteRead++;
        if (code != 0x1b)  // 0x1b in ascii is ESC (escape)
        {
            continue;
        }
        
        unsigned char type   = bytestream_get_byte(&(*buf)); 
        unsigned char dataLength = bytestream_get_byte(&(*buf));
        byteRead += 2 + dataLength;

        switch (type) 
        {
            case BD_TEXTST_DATA_STRING:
            {
                if(lineData->mText != NULL)
                {
                    for (int j = 0; j < dataLength; j++)
                    {
                        /* Read text string */
                        lineData->mText[j+textStringOffset] = (*buf)[j];
                    }
                }

                av_log(NULL, AV_LOG_ERROR,"%s",lineData->mText);
                /* Adjust string offset */
       //         buf += dataLength;
                (*buf) += dataLength;
                textStringOffset += dataLength;
                break;
            }

            case BD_TEXTST_DATA_NEWLINE:
            {
                // a new line coming, reset the counter
                textStringOffset      = 0;
                inlineStyleOffset     = 0;
                numberOfInlineStyles  = 0;
                // store the subtitle's lenght
    //            lineData->mTextLength = textStringOffset;
                textStringOffset = 0;
                
                // store the text subtitle
                region->mLineData[region->mLineCount-1] = lineData;
                region->mLineCount ++;
                // malloc new TextLineData
                lineData = av_malloc(sizeof(TextLineData));
                #if 0
                if(lineData != NULL)
                {
                    lineData->mText = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
                    lineData->mInLineStyleValues = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
                    lineData->mInLineStyleType = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
                    lineData->mInLineStylePosition = av_malloc(sizeof(char)*MAX_CHARS_PER_DPS_REGION);
                    if(lineData->mText != NULL)
                    {
                        memset(lineData->mText, 0, MAX_CHARS_PER_DPS_REGION);
                    }
                    if(lineData->mInLineStylePosition != NULL)
                    {
                        memset(lineData->mInLineStylePosition, 0xff, MAX_CHARS_PER_DPS_REGION);
                    }
                }
                #endif

                initLineData(lineData);
                break;
            }
                
            case BD_TEXTST_DATA_FONT_ID:
            case BD_TEXTST_DATA_FONT_STYLE:
            case BD_TEXTST_DATA_FONT_SIZE:
            case BD_TEXTST_DATA_FONT_COLOR:
  //          case BD_TEXTST_DATA_NEWLINE:
            case BD_TEXTST_DATA_RESET_STYLE:
            {
                if(lineData != NULL)
                {
                    /* set the inline style data type */
                    if(lineData->mInLineStyleType != NULL)
                    {
                        lineData->mInLineStyleType[numberOfInlineStyles] = type;
                    }

                    /* set the inline style position */
                    if(lineData->mInLineStylePosition != NULL)
                    {
                        lineData->mInLineStylePosition[numberOfInlineStyles] = textStringOffset;
                    }

                    if(lineData->mInLineStyleValues != NULL)
                    {
                        for (int j = 0; j < dataLength; j++)
                        {
                            /* Read inline style values */
                            lineData->mInLineStyleValues[j+inlineStyleOffset] =  (*buf)[j];
                        }
                    }
                }

                (*buf) += dataLength;

                /* Adjust inline style offset */
                inlineStyleOffset += dataLength;

                /* increment number of inline styles */
                numberOfInlineStyles++;
                break;
            }
                
            default:
            {
                av_log(NULL, AV_LOG_ERROR,"decodeDialogRegion(): unknown type %d (len %d)\n", type, length);
                (*buf) += length;
                continue;
            }
        }
    }

    region->mLineData[region->mLineCount-1] = lineData;
    
    return 0;
}

static void initDialogPresention(DialogRegion* region)
{
    if(region != NULL)
    {
        region->mContinousPresentFlag = 0;
        region->mForceOnFlag = 0;
        region->mRegionStyleRefId = 0;
        region->mLineCount = 0;
        for(int i = 0; i < MAX_TEXTSUBTITLE_LINE; i++)
        {
            region->mLineData[i] = NULL;
        }
    }
}

static int decodeDialogPresention(uint8_t** buf,DialogPresentation* dialog)
{
    if((buf == NULL) || (dialog == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention():param is invalid");
        return -1;
    }

    /* Set Start PTS value */
    int64_t temp = (bytestream_get_byte(&(*buf))&0x01)<<32;
    dialog->mStartPTS = bytestream_get_be32(&(*buf));
    dialog->mStartPTS |= temp;
    
    /* Set End PTS value */
    temp = (bytestream_get_byte(&(*buf))&0x01)<<32;
    dialog->mEndPTS = bytestream_get_be32(&(*buf));
    dialog->mEndPTS |= temp;//(uint64)(buffer[0] & 0x01) << 32;

//    av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention(), start PTS = %lld,sec = %d",dialog->mStartPTS,(dialog->mStartPTS/45000));
//    av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention(), end PTS = %lld,sec = %d",dialog->mEndPTS,(dialog->mEndPTS/45000));

    unsigned char flag = bytestream_get_byte(&(*buf));
    dialog->mPaletteUpdataFlag = flag>>7;
    av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention(), dialog->mPaletteUpdataFlag = %d",dialog->mPaletteUpdataFlag);
    dialog->mPalette = NULL;
    if(dialog->mPaletteUpdataFlag)
    {
        int length = bytestream_get_be16(&(*buf));
        int count = length/5;
        dialog->mPalette = av_malloc(sizeof(PaletteEntry));
        if(dialog->mPalette == NULL)
        {
            av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention():malloc mPaletteUpdate fail");
            return -1;
        }

        decodePalette(buf,count,dialog->mPalette);
    }

    dialog->mRegionCount = bytestream_get_byte(&(*buf));
    av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention(),RegionCount = %d",dialog->mRegionCount);
    dialog->mRegion = NULL;
    if(dialog->mRegionCount > 0)
    {
        dialog->mRegion = av_malloc(sizeof(DialogRegion)*dialog->mRegionCount);//new DialogRegion[dialog->mRegionCount];
        if(dialog->mRegion != NULL)
        {
            av_log(NULL, AV_LOG_ERROR,"decodeDialogPresention(),dialog->mRegion = 0x%x",dialog->mRegion);
//            memset(&dialog->mRegion,0,sizeof(DialogRegion)*dialog->mRegionCount);
            for(int i = 0; i < dialog->mRegionCount; i++)
            {
               initDialogPresention(&dialog->mRegion[i]);
               decodeDialogRegion(buf,&dialog->mRegion[i]);
            }
        }
    }
    
    return 0;
}


static int parseDialogPresentation(AVCodecContext *avctx,uint8_t *data,uint8_t* buffer,int length)
{
    if((avctx == NULL) || (buffer == NULL) || (data == NULL))
    {
        av_log(NULL, AV_LOG_ERROR,"parseDialogPresentation()unexpected dialog segment");
        return 0;
    }

    AVSubtitle *subtitle = (AVSubtitle *)data;
    subtitle->format = 1;
    subtitle->dialog = av_malloc(sizeof(DialogPresentation));
    if(subtitle->dialog  != NULL)
    {
        av_log(NULL, AV_LOG_ERROR,"text->mDialog address = 0x%x",subtitle->dialog);
        if(decodeDialogPresention(&buffer,  subtitle->dialog) != 0)
        {
            deleteTextSubtitleDialog(subtitle->dialog);
            return 0;
        }
    }
    else
    {
        return 0;
    }

    return 1;
}


static int decode(AVCodecContext *avctx, uint8_t *data, int *data_size,
                  AVPacket *avpkt)
{
    TextSubtitle *ctx = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    AVSubtitle *sub    = data;

    const uint8_t *buf_end;
    uint8_t       segment_type;
    int           segment_length;
    int i;


    *data_size = 0;
    
    /* Ensure that we have received at a least a segment code and segment length */
    if (buf_size < 3)
        return -1;

    buf_end = buf + buf_size;
        
    /* Step through buffer to identify segments */
    while (buf < buf_end) {
        segment_type   = bytestream_get_byte(&buf);
        segment_length = bytestream_get_be16(&buf);

        switch (segment_type) {
        case Text_DIALOG_STYLE:
            *data_size = parseDialogStyle(avctx,data,buf,segment_length);
            break;
            
        case Text_DIALOG_PRESENTATION:
            *data_size = parseDialogPresentation(avctx,data,buf,segment_length);
            break;
            
        default:
            av_log(avctx, AV_LOG_ERROR, "Unknown Text subtitle segment type 0x%x, length %d\n",
                   segment_type, segment_length);
            break;
        }

        buf += segment_length;
    }

    return buf_size;
}

static const AVOption options[] = {
 //   {"forced_subs_only", "Only show forced subtitles", OFFSET(forced_subs_only), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, SD},
    { NULL },
};

static const AVClass textSutbittle_class = {
    .class_name = "PGS subtitle decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


AVCodec ff_bluraytextsub_decoder = {
    .name           = "BlurayTextSubtitle",
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_BLURAY_TEXT_SUBTITLE,
    .priv_data_size = sizeof(TextSubtitle),
    .init           = init_decoder,
    .close          = close_decoder,
    .decode         = decode,
    .long_name      = NULL_IF_CONFIG_SMALL("blu-ray Text subtitles"),
    .priv_class     = &textSutbittle_class,
};

