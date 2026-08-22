/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <ctype.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>

#include "main.h"
#include "media/media.h"
#include "libav.h"
#include "fileaccess/fa_libav.h"
#include "video/video_decoder.h"
#include "video/video_settings.h"

#if ENABLE_VDPAU
#include "video/vdpau.h"
#endif




static const int libav_colorspace_tbl[] = {
  [AVCOL_SPC_BT709]     = COLOR_SPACE_BT_709,
  [AVCOL_SPC_BT470BG]   = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE170M] = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE240M] = COLOR_SPACE_SMPTE_240M,
};


#define vd_valid_duration(t) ((t) > 10000ULL && (t) < 1000000ULL)

static void init_filter_graph(AVFrame *frame, AVCodecContext *ctx, struct media_codec *mc)
{
    mc->filt_inputs  = avfilter_inout_alloc();
    mc->filt_outputs = avfilter_inout_alloc();

    char args[64];
    char description[64];

    snprintf(args, sizeof(args),
         "width=%d:height=%d:pix_fmt=%d:time_base=%d/%d:sar=%d/%d",
         frame->width,
         frame->height,
		 ctx->pix_fmt, ////frame->format,//
         ctx->time_base.num, //frame->height==480?30000:50,//ctx->time_base.num,
         ctx->time_base.den, //frame->height==480?1001:1,//ctx->time_base.den,
         ctx->sample_aspect_ratio.num,
         ctx->sample_aspect_ratio.den
		 );

    mc->filt_filter_graph = avfilter_graph_alloc();

    if(avfilter_graph_create_filter(&mc->filt_filter_src_ctx, avfilter_get_by_name("buffer"), "in", args, NULL, mc->filt_filter_graph)) goto leave_err;
    if(avfilter_graph_create_filter(&mc->filt_filter_sink_ctx, avfilter_get_by_name("buffersink"), "out", NULL, NULL, mc->filt_filter_graph)) goto leave_err;

    mc->filt_inputs->name        = av_strdup("out");
    mc->filt_inputs->filter_ctx  = mc->filt_filter_sink_ctx;
    mc->filt_inputs->pad_idx     = 0;
    mc->filt_inputs->next        = NULL;

    mc->filt_outputs->name       = av_strdup("in");
    mc->filt_outputs->filter_ctx = mc->filt_filter_src_ctx;
    mc->filt_outputs->pad_idx    = 0;
    mc->filt_outputs->next       = NULL;

	// https://ffmpeg.org/ffmpeg-filters.html#yadif-1

	sprintf(description, "bwdif=mode=%i:parity=%i", /*(mc->codec_id != AV_CODEC_ID_H264 && !gconf.disable_yadif_2x)?1:0,*/ 0, !frame->top_field_first);

	//TRACE(TRACE_DEBUG, "VD", "frame->interlaced_frame=%i frame->top_field_first=%i ctx->time_base.num=%i ctx->time_base.den=%i", frame->interlaced_frame, frame->top_field_first, ctx->time_base.num, ctx->time_base.den);
    //TRACE(TRACE_DEBUG, "avfilter", "Filter: %s %s", description, args);

    if(avfilter_graph_parse(mc->filt_filter_graph, description, mc->filt_inputs, mc->filt_outputs, NULL)) goto leave_err;
    if(avfilter_graph_config(mc->filt_filter_graph, NULL)) goto leave_err;

	TRACE(TRACE_DEBUG, "avfilter", "Filter graph connected: %s, %s", description, args);
    TRACE(TRACE_INFO, "avfilter", "Deinterlacer created and configured");
	mc->filt_filter_frame = av_frame_alloc();
	return;

leave_err:

	if(mc->filt_filter_graph) avfilter_graph_free(&mc->filt_filter_graph);
	return;
}

/**
 *
 */
static void
libav_deliver_frame(video_decoder_t *vd,
                    media_pipe_t *mp, media_queue_t *mq,
                    AVCodecContext *ctx, AVFrame *frame,
                    const media_buf_meta_t *mbm, int decode_time,
                    const media_codec_t *mc)
{
  frame_info_t fi;

  /* Compute aspect ratio */
  switch(mbm->mbm_aspect_override) {
  case 0:

    fi.fi_dar_num = frame->width;
    fi.fi_dar_den = frame->height;

    if(frame->sample_aspect_ratio.num) {
      fi.fi_dar_num *= frame->sample_aspect_ratio.num;
      fi.fi_dar_den *= frame->sample_aspect_ratio.den;
    } else if(mc->sar_num) {
      fi.fi_dar_num *= mc->sar_num;
      fi.fi_dar_den *= mc->sar_den;
    }

    break;
  case 1:
    fi.fi_dar_num = 4;
    fi.fi_dar_den = 3;
    break;
  case 2:
    fi.fi_dar_num = 16;
    fi.fi_dar_den = 9;
    break;
  }

  int64_t pts = video_decoder_infer_pts(mbm, vd,
					frame->pict_type == AV_PICTURE_TYPE_B);

  int duration = mbm->mbm_duration;

  if(!vd_valid_duration(duration)) {
    /* duration is zero or very invalid, use duration from last output */
    duration = vd->vd_estimated_duration;
  }

  if(pts == AV_NOPTS_VALUE && vd->vd_nextpts != AV_NOPTS_VALUE)
    pts = vd->vd_nextpts; /* no pts set, use estimated pts */

  if(pts != AV_NOPTS_VALUE && vd->vd_prevpts != AV_NOPTS_VALUE) {
    /* we know PTS of a prior frame */
    int64_t t = (pts - vd->vd_prevpts) / vd->vd_prevpts_cnt;

    if(vd_valid_duration(t)) {
      /* inter frame duration seems valid, store it */
      vd->vd_estimated_duration = t;
      if(duration == 0)
	duration = t;

    }
  }

  duration += frame->repeat_pict * duration / 2;

  if(pts != AV_NOPTS_VALUE) {
    vd->vd_prevpts = pts;
    vd->vd_prevpts_cnt = 0;
  }
  vd->vd_prevpts_cnt++;

  if(duration == 0) {
    TRACE(TRACE_DEBUG, "Video", "Dropping frame with duration = 0");
    return;
  }

  prop_set_int(mq->mq_prop_too_slow, decode_time > duration);

  if(pts != AV_NOPTS_VALUE) {
    vd->vd_nextpts = pts + duration;
  } else {
    vd->vd_nextpts = AV_NOPTS_VALUE;
  }
#if 0
  static int64_t lastpts = AV_NOPTS_VALUE;
  if(lastpts != AV_NOPTS_VALUE) {
    printf(" VDEC: %20"PRId64" : %-20"PRId64" %d %"PRId64" %6d %d epoch=%d\n", pts, pts - lastpts, mbm->mbm_drive_clock,
           mbm->mbm_user_time, duration, mbm->mbm_sequence, mbm->mbm_epoch);
#if 0
    if(pts - lastpts > 1000000) {
      abort();
    }
    #endif
  }
  lastpts = pts;
#endif


  media_discontinuity_debug(&vd->vd_debug_discont_out,
                            mbm->mbm_dts,
                            mbm->mbm_pts,
                            mbm->mbm_epoch,
                            mbm->mbm_skip,
                            "VOUT");

  vd->vd_interlaced |=
    frame->interlaced_frame && !mbm->mbm_disable_deinterlacer;

  fi.fi_width = frame->width;
  fi.fi_height = frame->height;
  fi.fi_pts = pts;
  fi.fi_epoch = mbm->mbm_epoch;
  fi.fi_user_time = mbm->mbm_user_time;
  fi.fi_duration = duration;
  fi.fi_drive_clock = mbm->mbm_drive_clock;

  fi.fi_interlaced = !!vd->vd_interlaced;
  fi.fi_tff = !!frame->top_field_first;
  fi.fi_prescaled = 0;

  fi.fi_color_space =
    ctx->colorspace < ARRAYSIZE(libav_colorspace_tbl) ?
    libav_colorspace_tbl[ctx->colorspace] : 0;

  fi.fi_type = 'LAVC';

  // Check if we should skip directly to convert code
  if(vd->vd_convert_width  != frame->width ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format) {

    // Nope, go ahead and deliver frame as-is

    fi.fi_data[0] = frame->data[0];
    fi.fi_data[1] = frame->data[1];
    fi.fi_data[2] = frame->data[2];

    fi.fi_pitch[0] = frame->linesize[0];
    fi.fi_pitch[1] = frame->linesize[1];
    fi.fi_pitch[2] = frame->linesize[2];

    fi.fi_pix_fmt = frame->format;
    fi.fi_avframe = frame;

    int r = video_deliver_frame(vd, &fi);

    /* return value
     * 0  = OK
     * 1  = Need convert to YUV420P
     * -1 = Fail
     */

    if(r != 1)
      return;
  }

  // Need to convert frame

  vd->vd_sws =
    sws_getCachedContext(vd->vd_sws,
                         frame->width, frame->height, frame->format,
                         frame->width, frame->height, AV_PIX_FMT_YUV420P,
                         0, NULL, NULL, NULL);

  if(vd->vd_sws == NULL) {
    TRACE(TRACE_ERROR, "Video", "Unable to convert from %s to %s",
	  av_get_pix_fmt_name(frame->format),
	  av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));
    return;
  }

  if(vd->vd_convert_width  != frame->width  ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format) {
    avpicture_free(&vd->vd_convert);

    vd->vd_convert_width  = frame->width;
    vd->vd_convert_height = frame->height;
    vd->vd_convert_pixfmt = frame->format;

    avpicture_alloc(&vd->vd_convert, AV_PIX_FMT_YUV420P, frame->width,
                    frame->height);

    TRACE(TRACE_DEBUG, "Video", "Converting from %s to %s",
	  av_get_pix_fmt_name(frame->format),
	  av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));
  }

  sws_scale(vd->vd_sws, (void *)frame->data, frame->linesize, 0,
            frame->height, vd->vd_convert.data, vd->vd_convert.linesize);

  fi.fi_data[0] = vd->vd_convert.data[0];
  fi.fi_data[1] = vd->vd_convert.data[1];
  fi.fi_data[2] = vd->vd_convert.data[2];

  fi.fi_pitch[0] = vd->vd_convert.linesize[0];
  fi.fi_pitch[1] = vd->vd_convert.linesize[1];
  fi.fi_pitch[2] = vd->vd_convert.linesize[2];

  fi.fi_type = 'LAVC';
  fi.fi_pix_fmt = AV_PIX_FMT_YUV420P;
  fi.fi_avframe = NULL;
  video_deliver_frame(vd, &fi);
}



/**
 *
 */
static void
libav_video_flush(media_codec_t *mc, video_decoder_t *vd)
{
  int got_pic = 0;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  AVPacket avpkt;

  av_init_packet(&avpkt);
  avpkt.data = NULL;
  avpkt.size = 0;

  while(1) {
    avcodec_decode_video2(ctx, vd->vd_frame, &got_pic, &avpkt);
    if(!got_pic)
      break;
    av_frame_unref(frame);
  };
  avcodec_flush_buffers(ctx);
}


/**
 *
 */
static void
libav_video_eof(media_codec_t *mc, video_decoder_t *vd,
                struct media_queue *mq)
{
  int got_pic = 0;
  media_pipe_t *mp = vd->vd_mp;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  AVPacket avpkt;
  int t;

  av_init_packet(&avpkt);
  avpkt.data = NULL;
  avpkt.size = 0;

  while(1) {

    avgtime_start(&vd->vd_decode_time);

    avcodec_decode_video2(ctx, vd->vd_frame, &got_pic, &avpkt);

    t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
                     mq->mq_prop_decode_peak);

    if(!got_pic)
      break;
    const media_buf_meta_t *mbm = &vd->vd_reorder[frame->reordered_opaque];
    if(!mbm->mbm_skip)
      libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);
    av_frame_unref(frame);
  };
  avcodec_flush_buffers(ctx);
}

#include "misc/minmax.h"

/**
 *
 */
static void
libav_decode_video(struct media_codec *mc, struct video_decoder *vd,
                   struct media_queue *mq, struct media_buf *mb, int reqsize)
{
  int got_pic = 0;
  media_pipe_t *mp = vd->vd_mp;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  int t;

  if(mb->mb_flush)
    libav_video_eof(mc, vd, mq);

  copy_mbm_from_mb(&vd->vd_reorder[vd->vd_reorder_ptr], mb);
  ctx->reordered_opaque = vd->vd_reorder_ptr;
  vd->vd_reorder_ptr = (vd->vd_reorder_ptr + 1) & VIDEO_DECODER_REORDER_MASK;

  /*
   * If we are seeking, drop any non-reference frames
   */
  ctx->skip_frame = mb->mb_skip == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
  avgtime_start(&vd->vd_decode_time);

  avcodec_decode_video2(ctx, frame, &got_pic, &mb->mb_pkt);

  t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
		   mq->mq_prop_decode_peak);

  mp_set_mq_meta(mq, ctx->codec, ctx);

  if(got_pic == 0)
    return;

  const media_buf_meta_t *mbm = &vd->vd_reorder[frame->reordered_opaque];
  if(mbm->mbm_skip) goto leave_d;

	if(frame->interlaced_frame
		&& (mc->codec_id == AV_CODEC_ID_MPEG2VIDEO || mc->codec_id == AV_CODEC_ID_MPEG1VIDEO || mc->codec_id == AV_CODEC_ID_H264 )
		&& (frame->width <= 1024)
		&& (frame->height <= 576)
	)
	{
		if(!mc->filt_filter_frame)
			init_filter_graph(frame, ctx, mc);

		if(mc->filt_filter_frame)
		{
			if(av_buffersrc_add_frame(mc->filt_filter_src_ctx, frame))
			{
				;
			}

			AVFrame *filter_frame2 = mc->filt_filter_frame;
			if(!av_buffersink_get_frame(mc->filt_filter_sink_ctx, filter_frame2))
			{
				libav_deliver_frame(vd, mp, mq, ctx, filter_frame2, mbm, t, mc);
				av_frame_unref(filter_frame2);

				if(mc->codec_id != AV_CODEC_ID_H264 && /* !gconf.disable_yadif_2x && */ !av_buffersink_get_frame(mc->filt_filter_sink_ctx, filter_frame2))
				{
					libav_deliver_frame(vd, mp, mq, ctx, filter_frame2, mbm, t, mc);
					av_frame_unref(filter_frame2);
				}
			}

			av_frame_unref(frame);
			return;
		}
	}

    libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);

leave_d:
  av_frame_unref(frame);
}


/**
 *
 */
static enum AVPixelFormat
libav_get_format(struct AVCodecContext *ctx, const enum AVPixelFormat *fmt)
{
  media_codec_t *mc = ctx->opaque;
  if(mc->close != NULL) {
    mc->close(mc);
    mc->close = NULL;
  }

#if ENABLE_VDPAU
  if(!vdpau_init_libav_decode(mc, ctx)) {
    return AV_PIX_FMT_VDPAU;
  }
#endif
  mc->get_buffer2 = &avcodec_default_get_buffer2;
  return avcodec_default_get_format(ctx, fmt);
}


/**
 *
 */
static int
get_buffer2_wrapper(struct AVCodecContext *s, AVFrame *frame, int flags)
{
  media_codec_t *mc = s->opaque;
  return mc->get_buffer2(s, frame, flags);
}

/**
 *
 */
static int
media_codec_create_lavc(media_codec_t *cw, const media_codec_params_t *mcp,
                        media_pipe_t *mp)
{
#if TARGET_OS_OSX || TARGET_OS_IPHONE
  /* Dolby Vision Profile 5 carries an IPT-PQ signal with no conventional
   * HDR10-compatible base layer.  If the earlier VideoToolbox codec factory
   * rejected Apple's Dolby-aware session, generic FFmpeg HEVC decoding is
   * not a valid fallback and would only produce incorrect/black video. */
  if(cw->codec_id == AV_CODEC_ID_HEVC && mcp != NULL &&
     mcp->dovi_valid && mcp->dovi_profile == 5) {
    TRACE(TRACE_INFO, "libav",
          "Refusing invalid software fallback for Dolby Vision Profile 5");
    return -1;
  }
#endif

  const AVCodec *codec = avcodec_find_decoder(cw->codec_id);

  if(codec == NULL)
    return -1;

  cw->ctx = avcodec_alloc_context3(codec);
  if(cw->fmt_ctx != NULL)
    avcodec_copy_context(cw->ctx, cw->fmt_ctx);

  // cw->ctx->debug = FF_DEBUG_PICT_INFO | FF_DEBUG_BUGS;

  if(mcp != NULL && mcp->extradata != NULL && !cw->ctx->extradata) {
    cw->ctx->extradata = calloc(1, mcp->extradata_size +
				AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(cw->ctx->extradata, mcp->extradata, mcp->extradata_size);
    cw->ctx->extradata_size = mcp->extradata_size;
  }

  if(mcp && mcp->cheat_for_speed)
    cw->ctx->flags2 |= AV_CODEC_FLAG2_FAST;

  if(codec->type == AVMEDIA_TYPE_VIDEO) {

    cw->get_buffer2 = &avcodec_default_get_buffer2;

    // If we run with vdpau and h264 libav will crash when going
    // back and forth between accelerated and non-accelerated mode
#if !ENABLE_WSL2
    if(!(video_settings.vdpau && cw->codec_id == AV_CODEC_ID_H264))
      cw->ctx->thread_count = gconf.concurrency;
#else
	if(cw->codec_id == AV_CODEC_ID_H264)
		cw->ctx->thread_safe_callbacks = 1;
	cw->ctx->thread_count = 4;
#endif
    cw->ctx->opaque = cw;
    cw->ctx->refcounted_frames = 1;
    cw->ctx->get_format = &libav_get_format;
    cw->ctx->get_buffer2 = &get_buffer2_wrapper;

    cw->decode = &libav_decode_video;
    cw->flush  = &libav_video_flush;
  }

  if(avcodec_open2(cw->ctx, codec, NULL) < 0) {
    TRACE(TRACE_INFO, "libav", "Unable to open codec %s",
	  codec ? codec->name : "<noname>");

    av_freep(&cw->ctx);

    return -1;
  }

  return 0;
}


REGISTER_CODEC(NULL, media_codec_create_lavc, 1000);

/**
 *
 */
media_format_t *
media_format_create(AVFormatContext *fctx)
{
  media_format_t *fw = malloc(sizeof(media_format_t));
  atomic_set(&fw->refcount, 1);
  fw->fctx = fctx;
  return fw;
}


/**
 *
 */
void
media_format_deref(media_format_t *fw)
{
  if(atomic_dec(&fw->refcount))
    return;
  fa_libav_close_format(fw->fctx, 0);
  free(fw);
}


/**
 *
 */
void
metadata_from_libav(char *dst, size_t dstlen,
		    const AVCodec *codec, const AVCodecContext *avctx)
{
  const char *name = codec->name;
  const char *profile = av_get_profile_name(codec, avctx->profile);

  if(codec->id == AV_CODEC_ID_DTS && profile != NULL)
    name = NULL;

  if(dstlen == 0)
    return;

  size_t off = 0;
  dst[0] = 0;

#define APPEND_METADATA(...) do {                                      \
    if(off < dstlen) {                                                  \
      int n__ = snprintf(dst + off, dstlen - off, __VA_ARGS__);         \
      if(n__ < 0)                                                       \
        return;                                                         \
      off += MIN((size_t)n__, dstlen - off - 1);                        \
    }                                                                   \
  } while(0)

  if(name) {
    APPEND_METADATA("%s", codec->name);
    char *n = dst;
    while(*n) {
      *n = toupper((unsigned char)*n);
      n++;
    }
  }

  if(profile != NULL)
    APPEND_METADATA("%s%s", off ? " " : "", profile);

  if(codec->id == AV_CODEC_ID_H264 && avctx->level != FF_LEVEL_UNKNOWN)
    APPEND_METADATA(" (Level %d.%d)",
                    avctx->level / 10, avctx->level % 10);

  if(avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    char buf[64];

    av_get_channel_layout_string(buf, sizeof(buf), avctx->channels,
                                 avctx->channel_layout);

    APPEND_METADATA(", %d Hz, %s", avctx->sample_rate, buf);
  }

  if(avctx->width)
    APPEND_METADATA(", %dx%d", avctx->width, avctx->height);

  if(avctx->hwaccel != NULL)
    APPEND_METADATA(" (%s)", avctx->hwaccel->name);

#undef APPEND_METADATA
}

/**
 *
 */
void
mp_set_mq_meta(media_queue_t *mq, const AVCodec *codec,
	       const AVCodecContext *avctx)
{
  if(mq->mq_meta_codec_id       == codec->id &&
     mq->mq_meta_profile        == avctx->profile &&
     mq->mq_meta_channels       == avctx->channels &&
     mq->mq_meta_channel_layout == avctx->channel_layout &&
     mq->mq_meta_width          == avctx->width &&
     mq->mq_meta_height         == avctx->height &&
     mq->mq_meta_color_transfer == avctx->color_trc)
    return;

  mq->mq_meta_codec_id       = codec->id;
  mq->mq_meta_profile        = avctx->profile;
  mq->mq_meta_channels       = avctx->channels;
  mq->mq_meta_channel_layout = avctx->channel_layout;
  mq->mq_meta_width          = avctx->width;
  mq->mq_meta_height         = avctx->height;
  mq->mq_meta_color_transfer = avctx->color_trc;

  if(mq->mq_meta_channel_layout == AV_CH_LAYOUT_STEREO ||
     mq->mq_meta_channels == 2)
	  prop_set_string(mq->mq_prop_aq, "2.0");
  else
  if(mq->mq_meta_channels==3)
	  prop_set_string(mq->mq_prop_aq, "2.1");
  else
  if(mq->mq_meta_channels==4)
	  prop_set_string(mq->mq_prop_aq, "4.0");
  else
  if(mq->mq_meta_channels==5)
	  prop_set_string(mq->mq_prop_aq, "5.0");
  else
  if(mq->mq_meta_channel_layout == AV_CH_LAYOUT_5POINT1 ||
     mq->mq_meta_channels == 6)
	  prop_set_string(mq->mq_prop_aq, "5.1");
  else
  if(mq->mq_meta_channel_layout == AV_CH_LAYOUT_7POINT1 ||
     mq->mq_meta_channels == 8)
	  prop_set_string(mq->mq_prop_aq, "7.1");
  else if(mq->mq_meta_channels < 1 || mq->mq_meta_channels > 8)
	  prop_set_string(mq->mq_prop_aq, NULL);

  char buf[128];
  metadata_from_libav(buf, sizeof(buf), codec, avctx);

  if(mq->mq_meta_width && mq->mq_meta_height)
  {
	  char buf2[24];

	  sprintf(buf2, "%s%s", ((avctx->field_order > AV_FIELD_PROGRESSIVE)?"i":""), " ");

	  if(strstr(buf, "H264"))
		  sprintf(buf, "AVC %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
	  else
	  if(strstr(buf, "HEVC"))
		  sprintf(buf, "HEVC %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
	  else
	  if(strstr(buf, "MPEG2"))
		  sprintf(buf, "MPEG2 %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
	  else
	  if(strstr(buf, "MPEG1"))
		  sprintf(buf, "MPEG1 %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
	  else
	  if(strstr(buf, "MPEG4"))
		  sprintf(buf, "MPEG4 %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
	  else
	  if(strstr(buf, "VP8") || strstr(buf, "VP9"))
		  sprintf(buf, "VP9 %ix%i%s", mq->mq_meta_width, mq->mq_meta_height, buf2);
  }

  prop_set_string(mq->mq_prop_codec, buf);

  if(mq->mq_meta_width && mq->mq_meta_height)
  {
	  if(mq->mq_meta_width>1920 || mq->mq_meta_height>1088)
		  prop_set_string(mq->mq_prop_vq, "4K");
	  else
	  if(mq->mq_meta_width>1280 || mq->mq_meta_height>720)
		  prop_set_string(mq->mq_prop_vq, "HD+");
	  else
	  if(mq->mq_meta_width>1024 || mq->mq_meta_height>576)
		  prop_set_string(mq->mq_prop_vq, "HD");
	  else
		  prop_set_string(mq->mq_prop_vq, "SD");
  }

  if(avctx->color_trc == AVCOL_TRC_ARIB_STD_B67)
    prop_set_string(mq->mq_prop_hdr, "HLG");
  else if(avctx->color_trc == AVCOL_TRC_SMPTE2084)
    prop_set_string(mq->mq_prop_hdr, "HDR");
  else
    prop_set_string(mq->mq_prop_hdr, NULL);

}
