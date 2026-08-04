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
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>

#include "fileaccess/fa_libav.h"
#include "media/media.h"
#include "backend/backend.h"
#include "misc/minmax.h"
#include "misc/str.h"
#include "misc/average.h"
#include "misc/bytestream.h"
#include "subtitles/video_overlay.h"
#include "text/text.h"
#include "main.h"

#include "hls.h"

#define PTS_MASK 0x1ffffffffLL

#define FMP4_READ_ALL 1

LIST_HEAD(ts_service_list, ts_service);
LIST_HEAD(ts_es_list, ts_es);

typedef struct ts_table {
  int tt_offset;
  int tt_lock;
  uint8_t *tt_data;
} ts_table_t;



static const AVRational mpeg_tc = {1, 90000};

//static const AVRational fmp4_tcv = {1, 1000000};
//static const AVRational fmp4_tca = {1, 1000000};

typedef struct ts_demuxer {
  struct ts_service_list td_services;
  struct ts_es_list td_elemtary_streams;
  ts_table_t td_pat;
  media_pipe_t *td_mp;
  hls_demuxer_t *td_hd;

  struct media_buf_queue td_packets;

  enum {
    TD_MUX_MODE_UNSET,
    TD_MUX_MODE_TS,
    TD_MUX_MODE_RAW,
#if USE_FMP4
	TD_MUX_MODE_FMP4
#endif
  } td_mux_mode;

  //uint8_t td_buf[8*188+AV_INPUT_BUFFER_PADDING_SIZE];//[86*188+AV_INPUT_BUFFER_PADDING_SIZE]; //1316 //2048 16356
  //uint8_t td_buf[188*7*1];//[86*188+AV_INPUT_BUFFER_PADDING_SIZE]; //1316 //2048 16356
  int td_buf_bytes;
  int td_last_r;

#if USE_FMP4

#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
#define TD_BUF_SIZE_V (8388560) // ~8MB = 8 * 1048570 (188 * 44620)
#else
#define TD_BUF_SIZE_V (4194280) // ~4MB = 4 * 1048570 (188 * 22310)
#endif
#define TD_BUF_SIZE_A  (524144)	// ~512KB = 2788 * 188

  //int td_muxed_audio;
  uint8_t *td_buf_fmp4;//[128*1024]; //2048 16356
  uint32_t td_buf_size;

#endif

} ts_demuxer_t;


typedef struct ts_service {
  LIST_ENTRY(ts_service) tss_link;
  uint16_t tss_pmtpid;
  ts_table_t tss_pmt;
  ts_demuxer_t *tss_demuxer;
} ts_service_t;


typedef struct ts_es {
  // Elementary Stream
  LIST_ENTRY(ts_es) te_link;
  uint16_t te_pid;

  uint8_t *te_buf;
  int te_buf_size;
  int te_packet_size;

  int te_data_type;
  int te_probe_frame;
  int te_stream;

  int te_duration;

  char te_logged_ts;
  char te_logged_keyframe;
  int te_logged_info;

  int64_t te_pts;
  int64_t te_dts;
  int64_t te_bts; // Base timestamp

  //int64_t te_start;

  media_codec_t *te_codec;

#if USE_FMP4
  AVCodecContext *h_ctx;
  AVFormatContext *h_fctx;
  int32_t time_bas;
  int32_t time_div;
  int time_ok;

  int32_t time_den;
  int32_t time_num;
  int32_t time_sr;

  AVInputFormat *h_fmt;
  AVIOContext *h_avio;
  fa_handle_t *h_fh;

#endif

  // Use to derive timestamps for files without timestamps
  int te_samples_per_frame;
  int te_sample_rate;
  int64_t te_samples;

  int te_current_seq;

  int64_t te_ts_offset;

  int te_last_seq;
  ext_subtitles_t *te_ess;


} ts_es_t;

	int64_t bw_start;
	float bw_size = 0;
	float bw_time = 0;

static void add_speed(prop_t* p, int64_t size)
{
	bw_time = (arch_get_avtime() - bw_start); bw_size += size;
	if(bw_time > 1000000)
	{
		//TRACE(TRACE_DEBUG, "speed", "fl=%.2f int=%i", (bw_size * 8.f / bw_time), (int)((bw_size * 8.f / bw_time) + 0.5f));
		prop_set_int(p, (bw_size * 8.f / bw_time) + 0.5f);
		bw_time = bw_size = 0;
		bw_start = arch_get_avtime();
	}
}

/**
 *
 */
static void
td_flush_packets(ts_demuxer_t *td)
{
  media_buf_t *mb;
  while((mb = TAILQ_FIRST(&td->td_packets)) != NULL) {
    TAILQ_REMOVE(&td->td_packets, mb, mb_link);
    media_buf_free_unlocked(td->td_mp, mb);
  }
}


/**
 *
 */
static void
te_destroy(ts_es_t *te)
{
  //TRACE(TRACE_INFO, "HLS", "te_destroy()");
  LIST_REMOVE(te, te_link);
  free(te->te_buf);

#if USE_FMP4
  //if(te->h_fctx != NULL)
  //{
	//fa_libav_close_format(te->h_fctx, 0);

	//avformat_close_input(&te->h_fctx);
	/*
	fa_close_with_park(te->h_avio->opaque, 1);
	av_free(te->h_avio->buffer);
	av_free(te->h_avio);
	*/
  //}
#endif

#if USE_FMP4
  //if(te->h_fh!=NULL) fa_close(te->h_fh);

  // memory LEAK for LIVE
  if(te->h_avio)
  {
	//TRACE(TRACE_ERROR, "HLS", "te_destroy - av_free...");
	//  fa_libav_close(te->h_avio); // we already closed the MAP url
	//avformat_close_input(&te->h_fctx);
	av_free(te->h_fctx);
	av_free(te->h_avio->buffer);
	av_free(te->h_avio);
  }

  te->h_avio = NULL;
  te->h_fctx = NULL;
  te->h_ctx = NULL;
  te->h_fh = NULL;
#endif

  if(te->te_codec != NULL)
  {
	  //TRACE(TRACE_ERROR, "HLS", "te_destroy - media_codec_deref");
	  media_codec_deref(te->te_codec);
  }

  free(te);
  //TRACE(TRACE_INFO, "HLS", "te_destroy() - sleeping");  sleep(5);
}


/**
 *
 */
static void
tss_destroy(ts_service_t *tss)
{
  LIST_REMOVE(tss, tss_link);
  free(tss->tss_pmt.tt_data);
  free(tss);
}


/**
 *
 */
static void
ts_demuxer_destroy(ts_demuxer_t *td)
{
  ts_es_t *te;
  ts_service_t *tss;

  td_flush_packets(td);

  while((te = LIST_FIRST(&td->td_elemtary_streams)) != NULL)
    te_destroy(te);

  while((tss = LIST_FIRST(&td->td_services)) != NULL)
    tss_destroy(tss);

  if(td->td_pat.tt_data!=NULL)
	free(td->td_pat.tt_data);

  /*if(td->td_hd->hd_hls->h_fctx_video != NULL)
	fa_libav_close_format(td->td_hd->hd_hls->h_fctx_video, 0);

  if(td->td_hd->hd_hls->h_fctx_audio != NULL)
	fa_libav_close_format(td->td_hd->hd_hls->h_fctx_audio, 0);
*/
#if USE_FMP4
  if(td->td_buf_size && td->td_buf_fmp4!=NULL)
    free(td->td_buf_fmp4);

  td->td_buf_size = 0;
  td->td_buf_fmp4 = NULL;
#endif
  free(td);
}

/**
 *
 */
static int
table_reassemble(ts_table_t *tt, const uint8_t *data, int len,
                 int pusi, void (*cb)(void *opaque, const uint8_t *data,
                                      int len), void *opaque)
{
  int excess, tsize;

  if(pusi) {
    tt->tt_offset = 0;
    tt->tt_lock = 1;
  }

  if(!tt->tt_lock)
    return -1;

  tt->tt_data = realloc(tt->tt_data, tt->tt_offset + len);
  memcpy(tt->tt_data + tt->tt_offset, data, len);
  tt->tt_offset += len;

  if(tt->tt_offset < 3)
    return len;

  tsize = 3 + (((tt->tt_data[1] & 0xf) << 8) | tt->tt_data[2]);

  if(tt->tt_offset < tsize)
    return len;

  excess = tt->tt_offset - tsize;

  tt->tt_offset = 0;
  if(tsize >= 7)
    cb(opaque, tt->tt_data + 3, tsize - 7);

  free(tt->tt_data);
  tt->tt_data = NULL;

  return len - excess;
}


/**
 *
 */
static void
parse_table(ts_table_t *tt, const uint8_t *tsb,
            void (*cb)(void *opaque, const uint8_t *data, int len),
            void *opaque)
{
  int off  = tsb[3] & 0x20 ? tsb[4] + 5 : 4;
  int pusi = tsb[1] & 0x40;

  if(off >= 188) {
    tt->tt_lock = 0;
    return;
  }

  if(pusi) {
    int len = tsb[off++];
    if(len > 0) {
      if(len > 188 - off) {
        tt->tt_lock = 0;
        return;
      }
      table_reassemble(tt, tsb + off, len, 0, cb, opaque);
      off += len;
    }
  }

  while(off < 188) {
    int r = table_reassemble(tt, tsb + off, 188 - off, pusi, cb, opaque);
    if(r < 0) {
      tt->tt_lock = 0;
      break;
    }
    off += r;
    pusi = 0;
  }
}


/**
 *
 */
static ts_service_t *
find_service(ts_demuxer_t *td, uint16_t pmtpid, int create)
{
  ts_service_t *tss;
  LIST_FOREACH(tss, &td->td_services, tss_link) {
    if(tss->tss_pmtpid == pmtpid)
      return tss;
  }
  if(create) {
    tss = calloc(1, sizeof(ts_service_t));
    tss->tss_demuxer = td;
    tss->tss_pmtpid = pmtpid;
    LIST_INSERT_HEAD(&td->td_services, tss, tss_link);
  }
  return tss;
}


/**
 *
 */
static ts_es_t *
find_es(ts_demuxer_t *td, uint16_t pid, int create)
{
  ts_es_t *te;
  //int pids=0;
  LIST_FOREACH(te, &td->td_elemtary_streams, te_link) {
	  //TRACE(TRACE_DEBUG, "HLS", "find_es(%i) | iter=%i", pid, pids); pids++;
    if(te->te_pid == pid)
      return te;
  }
  //TRACE(TRACE_DEBUG, "HLS", "find_es(%i) | NOT FOUND", pid);
  if(create) {
    te = calloc(1, sizeof(ts_es_t));
    te->te_pid = pid;
    te->te_pts = PTS_UNSET;
    te->te_dts = PTS_UNSET;
#if USE_FMP4
	te->h_fctx = NULL;
	te->h_ctx = NULL;
	te->h_avio = NULL;
#endif
    LIST_INSERT_HEAD(&td->td_elemtary_streams, te, te_link);
  }
  return te;
}


/**
 *
 */
static void
handle_pat(void *opaque, const uint8_t *ptr, int len)
{
  ts_demuxer_t *td = opaque;
  ptr += 5;
  len -= 5;
  while(len >= 4) {
    const uint16_t pid = (ptr[2] & 0x1f) << 8 | ptr[3];
    find_service(td, pid, 1);

    len -= 4;
    ptr += 4;
  }
}



/**
 *
 */
static void
handle_pmt(void *opaque, const uint8_t *ptr, int len)
{
  ts_service_t *tss = opaque;
  ts_demuxer_t *td = tss->tss_demuxer;
  hls_demuxer_t *hd = td->td_hd;
  hls_t *h = hd->hd_hls;
  int pid;
  int dllen;
  uint8_t dlen, estype;
  uint8_t dtag = 0;

  if(len < 9) {
    return;
  }

  //  service_id = ptr[0] << 8 | ptr[1];
  //  x          = (ptr[5] & 0x1f) << 8 | ptr[6];
  dllen      = (ptr[7] & 0xf) << 8 | ptr[8];

  ptr += 9;
  len -= 9;

  while(dllen > 1) {
    //dtag = ptr[0];

    dlen = ptr[1];
    len -= 2; ptr += 2; dllen -= 2;
    if(dlen > len) {
      return;
    }
    len -= dlen; ptr += dlen; dllen -= dlen;
  }

  while(len >= 5) {
    estype  = ptr[0];
    pid     = (ptr[1] & 0x1f) << 8 | ptr[2];
    dllen   = (ptr[3] & 0xf) << 8 | ptr[4];

	int dvb_type = 0;

    ptr += 5;
    len -= 5;

    char langbuf[4] = {0};
    const char *lang = NULL;

    while(dllen > 1) {
      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2;

      if(dlen > len)
	break;

// dtag
// https://dvb.org/wp-content/uploads/2024/09/A038r17_Specification-for-Service-Information-SI-in-DVB-Systems_Draft_EN_300-468-v1-19-1_September-2024.pdf

      switch(dtag) {
      case 0xa:	// audio lang
        if(dlen >= 3) {
          memcpy(langbuf, ptr, 3);
          lang = langbuf;
          break;
        }
	  case 0x56: // teletext sub lang
      case 0x59: // dvb lang
        if(dlen >= 3) {
          memcpy(langbuf, ptr, 3);
          lang = langbuf;
		  dvb_type = dtag;
          break;
        }
      }
      len -= dlen; ptr += dlen; dllen -= dlen;
    }

    ts_es_t *te = find_es(td, pid, 1);
    const char *name = NULL;
    if(te->te_codec == NULL) {

	  //TRACE(TRACE_DEBUG, "HLS", "Codec create - estype=%i dtag=%i", estype, dtag);
      const char *muxid;
      int hat_pid;
      if(hd == &h->h_primary) {
        muxid = NULL;
        hat_pid = pid;
      } else {
        muxid = hd->hd_current->hv_url;
        hat_pid = 0;
      }
      switch(estype) {

// https://en.wikipedia.org/wiki/Program-specific_information

      case 0x15:
      case 0x86:
        name = "data";
        break;

	  case 0x01:
		if(td->td_hd->hd_hls->h_codec_h264 != NULL && td->td_hd->hd_hls->h_codec_h264->codec_id==AV_CODEC_ID_MPEG1VIDEO)
		{
			//TRACE(TRACE_DEBUG, "HLS", "Codec ref MPEG1");
			te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
#if USE_SEEKBACK
			if(!(td->td_mp->mp_hls_source & 4)
#if defined(__ANDROID__)
				|| td->td_mp->mp_tunnel_mode
#endif
			)
			{
				//mp_flush(td->td_hd->hd_hls->h_codec_h264->mp);
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
			}
#endif
		}
		else
		{
		    media_codec_params_t mcp = {0};
			if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
			{
				mcp.width=hd->hd_current->hv_width;
				mcp.height=hd->hd_current->hv_height;
			}
			if(hd->hd_current && hd->hd_current->hv_fps)
			{
				mcp.frame_rate_num = ((float) hd->hd_current->hv_fps * 10000.f);
				mcp.frame_rate_den = 10000;
			}
			if(hd->hd_current && hd->hd_current->hv_sar_num)
			{
				mcp.sar_num = hd->hd_current->hv_sar_num;
				mcp.sar_den = hd->hd_current->hv_sar_den;
			}
#if defined(__ANDROID__)
			if(hd->hd_current && hd->hd_current->hv_field_order)
				mcp.field_order = hd->hd_current->hv_field_order;
#endif

			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_MPEG1VIDEO, 1, NULL, NULL, &mcp, td->td_mp);
			td->td_hd->hd_hls->h_codec_h264 = te->te_codec;
		}
        te->te_data_type = MB_VIDEO;
        te->te_stream = 0;
        name = "MPEG1";
        break;

      case 0x02:
		if(td->td_hd->hd_hls->h_codec_h264 != NULL && td->td_hd->hd_hls->h_codec_h264->codec_id==AV_CODEC_ID_MPEG2VIDEO)
		{
			//TRACE(TRACE_DEBUG, "HLS", "Codec ref MPEG2");
			te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
#if USE_SEEKBACK
			if(!(td->td_mp->mp_hls_source & 4)
#if defined(__ANDROID__)
				|| td->td_mp->mp_tunnel_mode
#endif
			)
			{
				//mp_flush(td->td_hd->hd_hls->h_codec_h264->mp);
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
			}
#endif
		}
		else
		{
		    media_codec_params_t mcp = {0};
			if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
			{
				mcp.width=hd->hd_current->hv_width;
				mcp.height=hd->hd_current->hv_height;
			}
			if(hd->hd_current && hd->hd_current->hv_fps)
			{
				mcp.frame_rate_num = ((float) hd->hd_current->hv_fps * 10000.f);
				mcp.frame_rate_den = 10000;
			}
			if(hd->hd_current && hd->hd_current->hv_sar_num)
			{
				mcp.sar_num = hd->hd_current->hv_sar_num;
				mcp.sar_den = hd->hd_current->hv_sar_den;
			}
#if defined(__ANDROID__)
			if(hd->hd_current && hd->hd_current->hv_field_order)
				mcp.field_order = hd->hd_current->hv_field_order;
#endif

			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_MPEG2VIDEO, 1, NULL, NULL, &mcp, td->td_mp);
			td->td_hd->hd_hls->h_codec_h264 = te->te_codec;
		}
        te->te_data_type = MB_VIDEO;
        te->te_stream = 0;
        name = "MPEG2";
        break;

      case 0x1b:
	  case 0x1f:
	  case 0x20:
        //te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
//        te->te_codec = media_codec_create(AV_CODEC_ID_H264, 1, NULL, NULL, NULL,
//                                          td->td_mp);

		if(td->td_hd->hd_hls->h_codec_h264 != NULL && td->td_hd->hd_hls->h_codec_h264->codec_id==AV_CODEC_ID_H264)
		{
			//TRACE(TRACE_DEBUG, "HLS", "Codec ref H264");
			te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
			//td->td_hd->hd_hls->h_codec_h264->avc_csd_epoch++;

			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
			/*media_buf_t *mb = media_buf_alloc_unlocked(td->td_hd->hd_hls->h_codec_h264->mp, 0);
			mb->mb_data_type = MB_CTRL_FLUSH;
			mb->mb_data32 = 0;
			TAILQ_INSERT_TAIL(&td->td_packets, mb, mb_link);
			*/
			//TRACE(TRACE_ERROR, "HLS", "------ QUEUE RESET/MERGE ----------");

#if USE_SEEKBACK
			if(!(td->td_mp->mp_hls_source & 4)
#if defined(__ANDROID__)
				|| td->td_mp->mp_tunnel_mode
#endif
			)
			{
				//mp_flush(td->td_hd->hd_hls->h_codec_h264->mp);
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
			}
#endif
		}
		else
		{
		    media_codec_params_t mcp = {0};
			if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
			{
				mcp.width=hd->hd_current->hv_width;
				mcp.height=hd->hd_current->hv_height;
			}
			if(hd->hd_current && hd->hd_current->hv_fps)
			{
				mcp.frame_rate_num = ((double) hd->hd_current->hv_fps * 1000.f);
				mcp.frame_rate_den = 1000;
			}
			if(hd->hd_current && hd->hd_current->hv_sar_num)
			{
				mcp.sar_num = hd->hd_current->hv_sar_num;
				mcp.sar_den = hd->hd_current->hv_sar_den;
			}
#if defined(__ANDROID__)
			if(hd->hd_current && hd->hd_current->hv_field_order)
				mcp.field_order = hd->hd_current->hv_field_order;
#endif

			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_H264, 1, NULL, NULL, &mcp, td->td_mp);
			td->td_hd->hd_hls->h_codec_h264 = te->te_codec;
		}
        te->te_data_type = MB_VIDEO;
        te->te_stream = 0;
        name = "AVC";
        break;

      case 0x24:
		if(td->td_hd->hd_hls->h_codec_h264 != NULL && td->td_hd->hd_hls->h_codec_h264->codec_id==AV_CODEC_ID_HEVC)
		{
			//TRACE(TRACE_DEBUG, "HLS", "Codec ref HEVC");
			te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
			// td->td_hd->hd_hls->h_codec_h264->avc_csd_epoch++;
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
			td->td_hd->hd_hls->h_codec_h264->mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
#if USE_SEEKBACK
			if(!(td->td_mp->mp_hls_source & 4)
#if defined(__ANDROID__)
				|| td->td_mp->mp_tunnel_mode
#endif
			)
			{
				//mp_flush(td->td_hd->hd_hls->h_codec_h264->mp);
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
			}
#endif
		}
		else
		{
		    media_codec_params_t mcp = {0};
			if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
			{
				mcp.width=hd->hd_current->hv_width;
				mcp.height=hd->hd_current->hv_height;
			}
			if(hd->hd_current && hd->hd_current->hv_fps)
			{
				mcp.frame_rate_num = ((double) hd->hd_current->hv_fps * 1000.f);
				mcp.frame_rate_den = 1000;
			}
			if(hd->hd_current && hd->hd_current->hv_sar_num)
			{
				mcp.sar_num = hd->hd_current->hv_sar_num;
				mcp.sar_den = hd->hd_current->hv_sar_den;
			}
#if defined(__ANDROID__)
			if(hd->hd_current && hd->hd_current->hv_field_order)
				mcp.field_order = hd->hd_current->hv_field_order;
#endif

			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_HEVC, 1, NULL, NULL, &mcp, td->td_mp);
			td->td_hd->hd_hls->h_codec_h264 = te->te_codec;
		}

        te->te_data_type = MB_VIDEO;
        te->te_stream = 0;
        name = "HEVC";
        break;

      case 0x0f:
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_AAC, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "AAC", NULL, 1);
        name = "AAC";
		te->te_probe_frame = 2;
        break;

      case 0x81:
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_AC3, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "AC3", NULL, 1);
        name = "AC3";
		te->te_probe_frame = 2;
        break;

      case 0x83:
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_TRUEHD, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "TRUEHD", NULL, 1);
        name = "TRUEHD";
		te->te_probe_frame = 2;
        break;

      case 0x84:
	  case 0x87:
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_EAC3, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "EAC3", NULL, 1);
        name = "EAC3";
		te->te_probe_frame = 2;
        break;

      case 0x85:
	  //case 0x86: // same as DATA
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_DTS, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "DTS", NULL, 1);
        name = "DTS";
		te->te_probe_frame = 2;
        break;

      case 0x03:
      case 0x04:
		td->td_mp->mp_hls_source |= 1;
        te->te_codec = media_codec_create(AV_CODEC_ID_MP3, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "MP3", NULL, 1);
        name = "MP3";
		te->te_probe_frame = 2;
        break;

      case 0x06:
		if(dvb_type == 0x59)
	    {
			//TRACE(TRACE_DEBUG, "HLS", "Codec create DVBSUB");
			td->td_mp->mp_hls_source |= 1;
			if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
			{
				media_codec_params_t mcp = {0};
				mcp.width=hd->hd_current->hv_width;
				mcp.height=hd->hd_current->hv_height;
				te->te_codec = media_codec_create(AV_CODEC_ID_DVB_SUBTITLE, 1, NULL, NULL, &mcp, td->td_mp);
			}
			else
				te->te_codec = media_codec_create(AV_CODEC_ID_DVB_SUBTITLE, 1, NULL, NULL, NULL, td->td_mp);

			te->te_data_type = MB_SUBTITLE;
			te->te_stream = hls_get_subtitle_track(h, hat_pid, muxid, lang, "DVBSUB", 1);
			name = "DVBSUB";
			break;
		}
		else
		if(dtag==0x6a)
		{
			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_AC3, 1, NULL, NULL, NULL,
											  td->td_mp);
			te->te_data_type = MB_AUDIO;
			te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "AC3", NULL, 1);
			name = "AC3";
			te->te_probe_frame = 2;
		}
		else
		if(dtag==0x7a)
		{
			//TRACE(TRACE_DEBUG, "HLS", "Codec create EAC3 - dvb_type=%i dtag=%i", dvb_type, dtag);
			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_EAC3, 1, NULL, NULL, NULL,
											  td->td_mp);
			te->te_data_type = MB_AUDIO;
			te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "EAC3", NULL, 1);
			te->te_probe_frame = 2;
			name = "EAC3";
			break;
		}
		else
		if(dtag==0x7b)
		{
			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_DTS, 1, NULL, NULL, NULL,
											  td->td_mp);
			te->te_data_type = MB_AUDIO;
			te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "DTS", NULL, 1);
			name = "DTS";
			te->te_probe_frame = 2;
			break;
		}
		else
		if(dtag==0x7c)
		{
			td->td_mp->mp_hls_source |= 1;
			te->te_codec = media_codec_create(AV_CODEC_ID_AAC, 1, NULL, NULL, NULL,
											  td->td_mp);
			te->te_data_type = MB_AUDIO;
			te->te_stream = hls_get_audio_track(h, hat_pid, muxid, lang, "AAC", NULL, 1);
			name = "AAC";
			te->te_probe_frame = 2;
			break;
		}
		/*
		if(dvb_type == 0x56)
	    {
			TRACE(TRACE_DEBUG, "HLS", "Codec create DVB TELETEXT");
			te->te_codec = media_codec_create(AV_CODEC_ID_DVB_TELETEXT, 1, NULL, NULL, NULL, td->td_mp);

			te->te_data_type = MB_SUBTITLE;
			te->te_stream = hls_get_subtitle_track(h, hat_pid, muxid, lang, "DVBTXT", 1);
			name = "DVBTXT";
			break;
		}
		*/

      default:
        break;
      }
    }

    if(!te->te_logged_info) {
      te->te_logged_info = 1;


      if(name == NULL) {
        TRACE(TRACE_ERROR, "HLS", "Unsupported estype 0x%x on pid %d in %s",
              estype, pid, td->td_hd->hd_type);
      } else {
        HLS_TRACE(h, "New %s TS PID %d type %s (0x%x) stream=%d",
                  td->td_hd->hd_type, pid, name ? name : "<unknown>", estype,
                  te->te_stream);
      }

    }
  }
}

#define getu8(b, l) ({ \
  uint8_t x = b[0];    \
  b+=1;                \
  l-=1;                \
  x;                   \
})


static int64_t
getpts(const uint8_t *p)
{
  int a =  p[0];
  int b = (p[1] << 8) | p[2];
  int c = (p[3] << 8) | p[4];

  if((a & 1) && (b & 1) && (c & 1)) {

    return
      ((int64_t)((a >> 1) & 0x07) << 30) |
      ((int64_t)((b >> 1)       ) << 15) |
      ((int64_t)((c >> 1)       ))
      ;

  } else {
    // Marker bits not present
    return PTS_UNSET;
  }
}


/**
 *
 */
static int
parse_pes_header(ts_es_t *te, const uint8_t *buf, size_t len)
{
  int64_t d;
  int hdr, flags, hlen;

  hdr   = getu8(buf, len);
  flags = getu8(buf, len);
  hlen  = getu8(buf, len);

  te->te_pts = PTS_UNSET;
  te->te_dts = PTS_UNSET;

  if(len < hlen || (hdr & 0xc0) != 0x80)
    return -1;

  if((flags & 0xc0) == 0xc0) {
    if(hlen < 10)
      return -1;

    te->te_pts = getpts(buf);
    te->te_dts = getpts(buf + 5);

    d = (te->te_pts - te->te_dts) & PTS_MASK;
    if(d > 6*90000) // 180000
	{
      // More than two seconds of PTS/DTS delta, PTS is probably corrupt
		//TRACE(TRACE_INFO, "HLS", "Audio delta: %lli", d);
      te->te_pts = PTS_UNSET;
	}

  } else if((flags & 0xc0) == 0x80) {
    if(hlen < 5)
      return -1;

    te->te_dts = te->te_pts = getpts(buf);
  } else
    return hlen + 3;

  return hlen + 3;
}



/**
 *
 */
inline static int64_t
rescale(int64_t ts)
{

  if(ts == AV_NOPTS_VALUE)
    return AV_NOPTS_VALUE;

  return av_rescale_q(ts, mpeg_tc, AV_TIME_BASE_Q);
}

/*
static int64_t
rescale_fmp4(int64_t ts, int type)
{

  if(ts == AV_NOPTS_VALUE)
    return AV_NOPTS_VALUE;

	if(type==MB_AUDIO)
		return av_rescale_q(ts, fmp4_tca, AV_TIME_BASE_Q);
	else
		return av_rescale_q(ts, fmp4_tcv, AV_TIME_BASE_Q);
}
*/

/**
 *
 */
static void
probe_duration(ts_es_t *te, uint8_t *data, int size)
{
  int got_frame = 0;
  AVPacket pkt = {
    .data = data,
    .size = size
  };

  //TRACE(TRACE_DEBUG,"HLS--", "probe_duration()");
  media_codec_t *mc = te->te_codec;

  AVCodec *codec = avcodec_find_decoder(mc->codec_id);
  if(codec == NULL) {
    te->te_probe_frame = 0;
	//TRACE(TRACE_DEBUG,"HLS--", "probe_duration() NO CODEC");
    return;
  }

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  ctx->thread_count = 3;

  if(avcodec_open2(ctx, codec, NULL) < 0) {
    av_freep(&ctx);
	//TRACE(TRACE_DEBUG,"HLS--", "avcodec_open2() FAILED");
    te->te_probe_frame = 0;
    return;
  }

  AVFrame *frame = av_frame_alloc();

  avcodec_decode_audio4(ctx, frame, &got_frame, &pkt);

  if(got_frame)
  {
	if(te->te_probe_frame == 1)
	{
		te->te_samples_per_frame = frame->nb_samples;
		te->te_sample_rate = frame->sample_rate;
		te->te_samples = 0;
	}

	if(mc->ctx)
	{
		//AVCodecContext *ctx_out = mc->ctx;
		mc->ctx->sample_rate = frame->sample_rate;
		mc->ctx->channels = ctx->channels;
		mc->ctx->channel_layout = frame->channel_layout;
		mc->ctx->bit_rate = ctx->bit_rate;

		mc->ctx->profile = ctx->profile;
		//if(gconf.debug_av_info)
		//	TRACE(TRACE_DEBUG, "TS-MUX-AUD", "HLS-TS Audio: %ich %iHz %ikb/s", (int)ctx->channels, (int)frame->sample_rate, (int)(ctx->bit_rate/1000));
	}
	te->te_probe_frame = 0;
  }
  //else TRACE(TRACE_DEBUG, "HLS--", "avcodec_decode_audio4() got_frame=%i", got_frame);

  avcodec_close(ctx);
  av_freep(&ctx);

  av_frame_free(&frame);
}


#if USE_FMP4
/**
 *
 */
static void
enqueue_packet_fmp4(ts_demuxer_t *td, const void *data, int len,
               int keyframe,
               hls_segment_t *hs, ts_es_t *te,
               hls_demuxer_t *hd)
{
	//hls_t *h = td->td_hd->hd_hls;

	//TRACE(TRACE_DEBUG, "HLS", ">>> type: %s  size: %i  seq: %i  key: %i  pts: %lli  dts: %lli",
	//					te->te_data_type==MB_VIDEO?"V":"A", len, te->te_current_seq, keyframe, te->te_pts, te->te_dts);

	int64_t user_time = PTS_UNSET;
	int drive_clock = 0;

	// Compute user time
	//if(te->te_pts != PTS_UNSET && te->te_pts != AV_NOPTS_VALUE)
	{
		//if(hs->hs_pts_offset == PTS_UNSET) hs->hs_pts_offset = 0;//te->te_pts;
		//if(hs->hs_dts_offset == PTS_UNSET) hs->hs_dts_offset = 0;//te->te_dts;

		user_time = hs->hs_time_offset + te->te_pts;
		drive_clock = te->te_data_type == MB_VIDEO;
	}

	//TRACE(TRACE_DEBUG, "HLS", "<<< type: %s seq: %i key: %i pts: %lli dts: %lli user: %lli",
	//				te->te_data_type==MB_VIDEO?"V":"A", seq, keyframe, pts, dts, user_time);

#if 0
static int64_t prev_pts = 0;
if(hs->hs_time_offset + te->te_pts - prev_pts < 0 && te->te_data_type == MB_AUDIO)
{
	TRACE(TRACE_ERROR, "HLS", "pts=%lli (%lli)", hs->hs_time_offset + te->te_pts, hs->hs_time_offset + te->te_pts - prev_pts);
	//hs->hs_time_offset += (prev_pts - hs->hs_time_offset - te->te_pts);
}
#endif

	media_buf_t *mb = media_buf_alloc_unlocked(td->td_mp, len);
	memcpy(mb->mb_data, data, len);
	mb->mb_user_time = user_time;
	mb->mb_dts = hs->hs_time_offset + te->te_dts; //(te->te_dts == AV_NOPTS_VALUE || hs->hs_time_offset == PTS_UNSET) ? AV_NOPTS_VALUE : (hs->hs_time_offset + MAX(te->te_dts - hs->hs_ts_offset, 0));
	mb->mb_pts = hs->hs_time_offset + te->te_pts; //(te->te_pts == AV_NOPTS_VALUE || hs->hs_time_offset == PTS_UNSET) ? AV_NOPTS_VALUE : (hs->hs_time_offset + MAX(te->te_pts - hs->hs_ts_offset, 0));
	mb->mb_drive_clock = drive_clock;
	mb->mb_cw = media_codec_ref(te->te_codec);

	//if(te->te_data_type == MB_AUDIO) prev_pts = mb->mb_pts;

	mb->mb_data_type = te->te_data_type;
	mb->mb_keyframe = keyframe;
	//if(te->te_data_type == MB_VIDEO) TRACE(TRACE_DEBUG, "HLS", "<<< mb->mb_keyframe = %i", keyframe);
	mb->mb_sequence = te->te_current_seq;

	mb->mb_duration = te->te_duration;

	mb->mb_stream = te->te_stream;
	TAILQ_INSERT_TAIL(&td->td_packets, mb, mb_link);
}
#endif

/**
 *
 */
static void
enqueue_packet(ts_demuxer_t *td, const void *data, int len,
               int64_t dts, int64_t pts, int keyframe,
               hls_segment_t *hs, int seq, ts_es_t *te,
               hls_demuxer_t *hd)
{
	hls_t *h = td->td_hd->hd_hls;

	int64_t dts_in = dts;

	/*
	if(pts!=AV_NOPTS_VALUE)
	{
		if(te->te_bts<dts && !te->te_bts) te->te_bts = dts;
		pts -= te->te_bts;
		dts -= te->te_bts;
		//TRACE(TRACE_DEBUG, "HLS", "<<< type: %s  seq: %i  key: %i  pts: %lli  dts: %lli  bts: %lli",
		//				te->te_data_type==MB_VIDEO?"V":"A", seq, keyframe, pts, dts, rescale(te->te_bts));
	}
	*/

	dts = rescale(dts);
	pts = rescale(pts);

	int64_t user_time = PTS_UNSET;
	int drive_clock = 0;

	if(hs != NULL) {
		hls_discontinuity_segment_t *hds = hs->hs_discontinuity_segment;

    // Compute user time
    if(pts != PTS_UNSET)
	{
		if(hs->hs_ts_offset == PTS_UNSET) hs->hs_ts_offset = pts;
		user_time = hs->hs_time_offset + MAX(pts - hs->hs_ts_offset, 0);
		drive_clock = te->te_data_type == MB_VIDEO;
    }

    if(hds->hds_offset == PTS_UNSET) {
      if(dts != PTS_UNSET) {
        if(hd->hd_last_dts != PTS_UNSET) {
			hds->hds_offset = hd->hd_last_dts - dts;
        } else {
			hds->hds_offset = 0;
        }
        HLS_TRACE(h, "Discontinuity segment %d gets "
                  "timestamp offset: %"PRId64, hds->hds_seq, hds->hds_offset);
      }
    }

    if(hds->hds_offset != PTS_UNSET) {

      if(dts != PTS_UNSET) {
        dts += hds->hds_offset;
        hd->hd_last_dts = dts;
      }

      if(pts != PTS_UNSET)
        pts += hds->hds_offset;


    } else {
      pts = PTS_UNSET;
      dts = PTS_UNSET;
    }
  } else {
    pts = PTS_UNSET;
    dts = PTS_UNSET;
  }

	if(te->te_sample_rate == 0)
	{
		media_pipe_t *mp = td->td_mp;
		media_queue_t *mq;

		if(te->te_data_type == MB_VIDEO)
			mq = &mp->mp_video;
		else
			mq = &mp->mp_audio;

		if(!(mq->mq_demuxer_flags & HLS_QUEUE_KEYFRAME_SEEN))
		{
			if(dts == PTS_UNSET) return;
			if(!keyframe)		return;
		}
	}

	//TRACE(TRACE_DEBUG, "HLS", "enqueue_packet(%i)", len);

	//TRACE(TRACE_DEBUG, "HLS", "<<< type: %s seq: %i key: %i pts: %lli dts: %lli user: %lli",
	//				te->te_data_type==MB_VIDEO?"V":"A", seq, keyframe, pts, dts, user_time);

  media_buf_t *mb = media_buf_alloc_unlocked(td->td_mp, len);
  memcpy(mb->mb_data, data, len);
  mb->mb_user_time = user_time;
  mb->mb_dts = dts;
  mb->mb_pts = pts;
  mb->mb_drive_clock = drive_clock;
  mb->mb_cw = media_codec_ref(te->te_codec);

  mb->mb_duration = rescale(dts_in - te->te_codec->parser_ctx->last_dts);

  mb->mb_data_type = te->te_data_type;
  mb->mb_keyframe = keyframe;
  mb->mb_sequence = seq;

  if(mb->mb_keyframe && !te->te_logged_keyframe) {
    te->te_logged_keyframe = 1;
    HLS_TRACE(h,
              "%s        keyframe %20"PRId64":%20"PRId64" demuxer:%s stream:%d\n",
              te->te_data_type == MB_VIDEO ? "VIDEO" : (te->te_data_type == MB_AUDIO ? "AUDIO" : "SUBTITLE"),
              te->te_codec->parser_ctx->dts,
              te->te_codec->parser_ctx->pts,
              hd->hd_type,
              te->te_stream);
  }

  mb->mb_stream = te->te_stream;
  TAILQ_INSERT_TAIL(&td->td_packets, mb, mb_link);
}

#if USE_FMP4
static void
parse_data_fmp4(ts_demuxer_t *td, hls_variant_t *hv, ts_es_t *te,
           const uint8_t *data, int size, int keyframe)
{
	if(data == NULL || !size) return;
	hls_segment_t *hs  = hv != NULL ? hv_find_segment_by_seq(hv, te->te_current_seq) : NULL;
	if(hs==NULL || hv==NULL) return;

	if(te->te_codec->codec_id == AV_CODEC_ID_AV1)
	{
		media_codec_t *mc = te->te_codec;

		if(!mc->parser_ctx)
		{
			mc->parser_ctx = av_parser_init(mc->codec_id);
			//TRACE(TRACE_ERROR, "HLS", "NO PARSER - created");
		}

		uint8_t *outbuf;
		int outlen;

		av_parser_parse2(mc->parser_ctx, te->h_ctx, &outbuf, &outlen,
								data, size, te->te_pts, te->te_dts,
								te->te_current_seq);

		//TRACE(TRACE_DEBUG, "HLS", "av_parser_parse2()=%i outlen=%i", rlen, outlen);
		//if(outlen && mc->parser_ctx->key_frame)
		//	TRACE(TRACE_DEBUG, "HLS", "mc->parser_ctx->key = %i (set=%i)", mc->parser_ctx->key_frame, (outlen && mc->parser_ctx->key_frame));
		enqueue_packet_fmp4(td, data, size, (outlen && mc->parser_ctx->key_frame), hs, te, td->td_hd);
		return;

		/*if(mc->parser_ctx != NULL)
		{
			//TRACE(TRACE_ERROR, "HLS", "mc->parser_ctx - close");
			//av_parser_close(mc->parser_ctx);
			//mc->parser_ctx = NULL;
		}*/
	}

	enqueue_packet_fmp4(td, data, size, keyframe, hs, te, td->td_hd);
	return;
}
#endif

/**
 *
 */
static void
parse_data(ts_demuxer_t *td, hls_variant_t *hv, ts_es_t *te,
           const uint8_t *data, int size)
{

#if USE_FMP4
	if(td->td_mux_mode == TD_MUX_MODE_FMP4) return;
#endif

  media_codec_t *mc = te->te_codec;

  if(mc == NULL)
    return;

  while(size > 0 || data == NULL) {

    uint8_t *outbuf;
    int outlen;
    int rlen;


    rlen = av_parser_parse2(mc->parser_ctx, mc->fmt_ctx, &outbuf, &outlen,
                            data, size, te->te_pts, te->te_dts,
                            te->te_current_seq);


    if(outlen) {

      int64_t dts        = mc->parser_ctx->dts;
      int64_t pts        = mc->parser_ctx->pts;
      int     seq        = mc->parser_ctx->pos;
      const int keyframe = mc->parser_ctx->key_frame;

      if(seq == -1)
        seq = te->te_last_seq;
      else
        te->te_last_seq = seq;

      hls_segment_t *hs  = hv != NULL ? hv_find_segment_by_seq(hv, seq) : NULL;

	  if(te->te_probe_frame)
	  {
        // Need to find actual duration by decoding a frame
        probe_duration(te, outbuf, outlen);
      }

      if(te->te_sample_rate != 0) {
        AVRational timebase = {1, te->te_sample_rate};
        int64_t ts = av_rescale_q(te->te_samples, timebase, mpeg_tc);
        dts = pts = te->te_bts + ts;
		te->te_samples += te->te_samples_per_frame;
      }

	//if(te->te_data_type == MB_AUDIO)
	//	TRACE(TRACE_DEBUG, "HLS", "mc->parser_ctx->dts_sync_point=%i", te->te_codec->parser_ctx->dts_sync_point);
	//if(te->te_codec->parser_ctx->dts_sync_point<0)
	//	te->te_codec->parser_ctx->dts_sync_point = dts;
	//if(te->te_data_type == MB_VIDEO)
	//TRACE(TRACE_DEBUG, "HLS", "mc->parser_ctx->last_dts=%lli d:%lli r:%lli",te->te_codec->parser_ctx->last_dts, pts-mc->parser_ctx->last_dts, rescale(dts-mc->parser_ctx->last_dts));

      enqueue_packet(td, outbuf, outlen, dts, pts, keyframe, hs, seq, te,
                     td->td_hd);
    }

    te->te_pts = PTS_UNSET;
    te->te_dts = PTS_UNSET;

    if(data == NULL) {
      if(outlen == 0)
        return;
      continue;
    }

    data += rlen;
    size -= rlen;
  }
}


/**
 *
 */
static void
drain_parsers(ts_demuxer_t *td)
{
  ts_es_t *te;
  while((te = LIST_FIRST(&td->td_elemtary_streams)) != NULL) {

    if(te->te_codec != NULL)
	{
#if USE_FMP4
		if(td->td_mux_mode != TD_MUX_MODE_FMP4)
#endif
		parse_data(td, NULL, te, NULL, 0);
	}

    te_destroy(te);
  }
}


/**
 *
 */
static void
emit_packet(ts_es_t *te, ts_demuxer_t *td, hls_segment_t *hs)
{
  const uint8_t *data = te->te_buf;
  int            size = te->te_packet_size;

  if(size < 9)
    return;

  data += 6;
  size -= 6;

  int hlen = parse_pes_header(te, data, size);
  if(hlen < 0)
    return;

  if(!te->te_logged_ts && te->te_pts != PTS_UNSET) {
    te->te_logged_ts = 1;
    HLS_TRACE(hs->hs_variant->hv_demuxer->hd_hls,
              "%s First timestamp %20"PRId64":%20"PRId64"\n",
              te->te_data_type == MB_VIDEO ? "VIDEO" : (te->te_data_type == MB_AUDIO ? "AUDIO" : "SUBTITLE"),
              te->te_dts, te->te_pts);
  }

  data += hlen;
  size -= hlen;


#if 0 // Not in use
  if(te->te_data_type == MB_VIDEO && 0)
    parse_h264(td, te, data, size, hs, hv);
  else
#endif
    parse_data(td, hs->hs_variant, te, data, size);
}


/**
 *
 */
static void
process_es(ts_es_t *te, const uint8_t *tsb, ts_demuxer_t *td, hls_segment_t *hs)
{
  int off             = tsb[3] & 0x20 ? tsb[4] + 5 : 4;
  int pusi            = tsb[1] & 0x40;
  const uint8_t *data = tsb + off;
  int size            = 188 - off;

//TRACE(TRACE_DEBUG, "HLS", "size=%i off=%i", size, off);
  if(size<=0) return;

  if(pusi) {
    if(te->te_buf != NULL)
      memset(te->te_buf + te->te_packet_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    emit_packet(te, td, hs);
    te->te_packet_size = 0;
    te->te_current_seq = hs->hs_seq;
  }

  if(te->te_packet_size + size > te->te_buf_size) {
    te->te_buf_size = te->te_buf_size * 2 + size;
    te->te_buf = myreallocf(te->te_buf,
                            te->te_buf_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if(te->te_buf == NULL) {
      te->te_buf_size = 0;
      return;
    }
  }

  memcpy(te->te_buf + te->te_packet_size, data, size);
  te->te_packet_size += size;
}


/**
 *
 */
static void
process_tsb(ts_demuxer_t *td, const uint8_t *tsb, hls_segment_t *hs)
{
  ts_service_t *tss;
  ts_es_t *te;
  if(tsb[0] != 0x47)
    return;

  const unsigned int pid = (tsb[1] & 0x1f) << 8 | tsb[2];

  if(pid == 0) {
    parse_table(&td->td_pat, tsb, handle_pat, td);
    return;
  }

  LIST_FOREACH(tss, &td->td_services, tss_link) {
    if(pid == tss->tss_pmtpid) {
      parse_table(&tss->tss_pmt, tsb, handle_pmt, tss);
      return;
    }
  }

  LIST_FOREACH(te, &td->td_elemtary_streams, te_link) {
    if(pid == te->te_pid) {
      process_es(te, tsb, td, hs);
      return;
    }
  }

}



/**
 *
 */
static media_buf_t *
get_pkt(ts_demuxer_t *td)
{
  media_buf_t *mb;

  mb = TAILQ_FIRST(&td->td_packets);
  if(mb != NULL)
    TAILQ_REMOVE(&td->td_packets, mb, mb_link);

  return mb;
}


/**
 *
 */
static void
hls_ts_demuxer_close(hls_variant_t *hv)
{
  ts_demuxer_t *td = hv->hv_demuxer_private;
  assert(td != NULL);

  ts_demuxer_destroy(td);
  hv->hv_demuxer_private = NULL;
  hv->hv_demuxer_close = NULL;
  hv->hv_demuxer_flush = NULL;
}


/**
 *
 */
static void
hls_ts_demuxer_flush(hls_variant_t *hv)
{
	ts_demuxer_t *td = hv->hv_demuxer_private;
	ts_es_t *te;

	LIST_FOREACH(te, &td->td_elemtary_streams, te_link)
	{
		media_codec_t *mc = te->te_codec;
		if(mc == NULL)
			continue;
		if(mc->parser_ctx!=NULL)
		{
			//TRACE(TRACE_ERROR, "HLS", "mc->parser_ctx close");
			av_parser_close(mc->parser_ctx);
			mc->parser_ctx = av_parser_init(mc->codec_id);
		}

		te->te_packet_size = 0;
		te->te_pts = PTS_UNSET;
		te->te_dts = PTS_UNSET;
		te->te_last_seq = 0;
	}
	td_flush_packets(td);
}



/**
 *
 */
static void
unmuxed_input(ts_demuxer_t *td, const uint8_t *buf, int len, hls_segment_t *hs)
{
  ts_es_t *te = LIST_FIRST(&td->td_elemtary_streams);
  assert(te != NULL);

  te->te_current_seq = hs->hs_seq;

  parse_data(td, hs->hs_variant, te, buf, len);

#if 0 // test without parser
  media_codec_t *mc = te->te_codec;

  if(mc == NULL)
    return;

      te->te_pts = te->te_dts = te->te_bts;

      enqueue_packet(td, buf, len, te->te_pts, te->te_dts, 1, hs, hs->hs_seq, te,
                     td->td_hd);


    te->te_pts = PTS_UNSET;
    te->te_dts = PTS_UNSET;
#endif
}

/**
 *
 */

static video_overlay_t*
es_insert_text(ext_subtitles_t *es, const char *text,
	       int64_t start, int64_t stop, int tags)
{
  video_overlay_t *vo;
  vo = video_overlay_render_cleartext(text, start, stop, tags, 0, NULL);
  //if(vo != NULL)
  //  TAILQ_INSERT_TAIL(&es->es_entries, vo, vo_link);
  return vo;
}


/**
 *
 */
typedef struct {
  const char *buf;
  size_t len;       // Remaining bytes in buf
  ssize_t ll;        // Length of current line
} linereader_t;


static void
linereader_init(linereader_t *lr, const char *buf, size_t len)
{
  lr->buf = buf;
  lr->len = len;
  lr->ll = -1;
}



/**
 * Find end of line
 */
static ssize_t
linereader_next(linereader_t *lr)
{
  ssize_t i;

  if(lr->ll != -1) {
    // Skip over previous line
    lr->buf += lr->ll;
    lr->len -= lr->ll;

    /* Skip over EOL */
    if(lr->len > 0 && lr->buf[0] == 13) {
      lr->len--;
      lr->buf++;
    }

    if(lr->len > 0 && lr->buf[0] == 10) {
      lr->len--;
      lr->buf++;
    }

  }

  if(lr->len == 0) {
    /* At EOF */
    lr->ll = -1;
    return -1;
  }

  for(i = 0; i < lr->len; i++)
    if(lr->buf[i] == 10 || lr->buf[i] == 13)
      break;

  lr->ll = i;
  return i;
}



/**
 *
 */
static int64_t
get_srt_timestamp2(const char *buf)
{
  return 1000LL * (
    (buf[ 0] - '0') * 36000000LL +
    (buf[ 1] - '0') *  3600000LL +
    (buf[ 3] - '0') *   600000LL +
    (buf[ 4] - '0') *    60000LL +
    (buf[ 6] - '0') *    10000LL +
    (buf[ 7] - '0') *     1000LL +
    (buf[ 9] - '0') *      100LL +
    (buf[10] - '0') *       10LL +
    (buf[11] - '0'));
}

// 59:59.182
static int64_t
get_srt_timestamp3(const char *buf)
{
  return 1000LL * (
//    (buf[ 0] - '0') * 36000000LL +
//    (buf[ 1] - '0') *  3600000LL +
    (buf[ 0] - '0') *   600000LL +
    (buf[ 1] - '0') *    60000LL +
    (buf[ 3] - '0') *    10000LL +
    (buf[ 4] - '0') *     1000LL +
    (buf[ 6] - '0') *      100LL +
    (buf[ 7] - '0') *       10LL +
    (buf[ 8] - '0'));
}

// 1:00:02.164
static int64_t
get_srt_timestamp4(const char *buf)
{
  return 1000LL * (
//    (buf[ 0] - '0') * 36000000LL +
    (buf[ 0] - '0') *  3600000LL +
    (buf[ 2] - '0') *   600000LL +
    (buf[ 3] - '0') *    60000LL +
    (buf[ 5] - '0') *    10000LL +
    (buf[ 6] - '0') *     1000LL +
    (buf[ 8] - '0') *      100LL +
    (buf[ 9] - '0') *       10LL +
    (buf[10] - '0'));
}

// 1:02.164
static int64_t
get_srt_timestamp5(const char *buf)
{
  return 1000LL * (
//    (buf[ 0] - '0') * 36000000LL +
//    (buf[ 0] - '0') *  3600000LL +
//    (buf[ 2] - '0') *   600000LL +
    (buf[ 0] - '0') *    60000LL +
    (buf[ 2] - '0') *    10000LL +
    (buf[ 3] - '0') *     1000LL +
    (buf[ 5] - '0') *      100LL +
    (buf[ 6] - '0') *       10LL +
    (buf[ 7] - '0'));
}

/**
 *
 */

/*
59:59.182 --> 01:00:04.712	#26 stamp3/2
01:00:02.212 --> 01:00:04.712	#29 stamp2/2

0:01.000 --> 0:04.000		#21 stamp5/5
0:01.000 --> 10:04.000		#22 stamp5/3
00:01.000 --> 00:04.000		#23 stamp3/3
59:59.182 --> 1:00:02.164	#25 stamp3/4
1:00:02.212 --> 1:00:04.712	#27 stamp4/4
*/
static int
get_srt_timestamp(linereader_t *lr, int64_t *start, int64_t *stop)
{
  if(lr->ll >= 29 && !memcmp(lr->buf + 12, " --> ", 5) && (lr->buf[25]=='.' || lr->buf[25]==','))
  {
	*start = get_srt_timestamp2(lr->buf);
	*stop  = get_srt_timestamp2(lr->buf + 17);
	return 0;
  }

  if(lr->ll >= 26 && !memcmp(lr->buf + 9, " --> ", 5) && (lr->buf[22]=='.' || lr->buf[22]==','))
  {
	*start = get_srt_timestamp3(lr->buf);
	*stop  = get_srt_timestamp2(lr->buf + 14);
	return 0;
  }

  if(lr->ll >= 21 && !memcmp(lr->buf + 8, " --> ", 5) && (lr->buf[17]=='.' || lr->buf[17]==','))
  {
	*start = get_srt_timestamp5(lr->buf);
	*stop  = get_srt_timestamp5(lr->buf + 13);
	return 0;
  }

  if(lr->ll >= 22 && !memcmp(lr->buf + 8, " --> ", 5) && (lr->buf[18]=='.' || lr->buf[18]==','))
  {
	*start = get_srt_timestamp5(lr->buf);
	*stop  = get_srt_timestamp3(lr->buf + 13);
	return 0;
  }

  if(lr->ll >= 23 && !memcmp(lr->buf + 9, " --> ", 5) && (lr->buf[19]=='.' || lr->buf[19]==','))
  {
	*start = get_srt_timestamp3(lr->buf);
	*stop  = get_srt_timestamp3(lr->buf + 14);
	return 0;
  }

  if(lr->ll >= 25 && !memcmp(lr->buf + 9, " --> ", 5) && (lr->buf[21]=='.' || lr->buf[21]==','))
  {
	*start = get_srt_timestamp3(lr->buf);
	*stop  = get_srt_timestamp4(lr->buf + 14);
	return 0;
  }

  if(lr->ll >= 27 && !memcmp(lr->buf + 11, " --> ", 5) && (lr->buf[23]=='.' || lr->buf[23]==','))
  {
	*start = get_srt_timestamp4(lr->buf);
	*stop  = get_srt_timestamp4(lr->buf + 16);
	return 0;
  }

  return -1;

}


/**
 *
 */
static void
srt_skip_preamble(const char **bufp, size_t *lenp)
{
  const char *buf = *bufp;
  size_t len = *lenp;

  // WEBVTT is srt (see #2752)
  if(len > 6 && !memcmp(buf, "WEBVTT", 6)) {
    buf += 6;
    len -= 6;
  }

  // Skip over any initial control characters (Issue #1885)
  while(len && *buf && *buf <= 32) {
    len--;
    buf++;
  }

  *bufp = buf;
  *lenp = len;
}

static void StrToSentence(char *string)
{
	int length=0,i=0,sos=1;
	length = strlen(string);

	for(i=0;i<length;i++)
		if((string[i]>='a' && string[i]<='z') || string[i]>127) return;

	for(i=0;i<length;i++)
	{
		if( (sos) && (string[i]>='a' && string[i]<='z'))
		{
			string[i] = string[i] - 32;
			sos = 0;
			continue;
		}

		if(string[i]=='.')
		{
			sos = 1;
			continue;
		}

		if( (!sos || i>0) && (string[i]>='A' && string[i]<='Z'))
		{
			string[i] = string[i] + 32;
			if(i>0 && i<(length-1) && string[i]=='i')
			{
				if((string[i-1]==' ' || string[i-1]==0x0a) && (string[i+1]==' ' || string[i+1]=='\''))
					string[i]='I';
			}
			continue;
		}
	}
}

static void
parse_vtt(const char* buf, size_t len, int64_t delay, media_pipe_t* mp)
{
		const int tag_flags = TEXT_PARSE_HTML_TAGS | TEXT_PARSE_HTML_ENTITIES |
			TEXT_PARSE_SLOPPY_TAGS | TEXT_PARSE_SUB_TAGS;

		int n;
		size_t tlen = 0;
		int64_t start = -1, stop = -1;
		static int64_t pstart = -1, pstop = -1;
		static int got_ts = 0;
		int lines = 0;

		char *txt = NULL;
		ext_subtitles_t *es = NULL;
		video_overlay_t *vo = NULL;

//TRACE(TRACE_ERROR, "HLS---", "LEN: %i | B: %s", len, buf);
		/*
		if(te->te_ess == NULL)
		{
			es = calloc(1, sizeof(ext_subtitles_t));
			te->te_ess = es;
			TAILQ_INIT(&es->es_entries);
		}
		else
			es = te->te_ess;
		*/

		size_t txtoff = 0;
		linereader_t lr;

		srt_skip_preamble(&buf, &len);
		linereader_init(&lr, buf, len);

		  while(1) {
			if((n = linereader_next(&lr)) < 0)
			  break;

			if(get_srt_timestamp(&lr, &start, &stop) == 0) {
			  got_ts = 1;
			  lines++;
			  if(txt != NULL && pstart != -1 && pstop != -1) {
				txt[txtoff] = 0;
				if((stop - start) < 1500000) stop = start + 1500000;
				if((stop - start) > 14000000) stop = start + 14000000;

				if((pstop - pstart) < (txtoff*82000)) pstop = pstart + (txtoff*82000) + 410000;
				if((pstop - pstart) < 1500000) pstop = pstart + 1500000;
				if((pstop - pstart) > 14000000) pstop = pstart + 14000000;

				if(start - pstop < 100000) pstop = start - 100000;

				if(pstop - pstart > 1500000 && txtoff>3)
				{
					if(txtoff>40 && strchr(txt, 0x0a) == NULL)
					{
						for(int cp=(int)(txtoff/2); cp<txtoff; cp++)
						{
							if(txt[cp] == 0x20)
							{
								txt[cp] = 0x0a;
								break;
							}
						}
					}
					StrToSentence(txt);
					//TRACE(TRACE_ERROR, "HLS---", "S: %lli | E: %lli | T: %s", pstart+delay, pstop+delay, txt);
					vo = es_insert_text(es, txt, pstart+delay, pstop+delay, tag_flags);
					if(vo != NULL)
						video_overlay_enqueue(mp, vo);

					/*
					video_overlay_t *vo;
					vo = video_overlay_render_cleartext(txt, pstart+delay, pstop+delay, tag_flags, 0, NULL);
					if(vo != NULL)
					{
						video_overlay_enqueue(mp, vo);
					}
					*/
				}

				free(txt);
				txt = NULL;
				tlen = 0;
				txtoff = 0;
			  }
				pstart = start;
				pstop  = stop;
			    continue;
			}

			if(pstart == -1 || (!got_ts && pstart != -1))
			  continue;

			txt = realloc(txt, tlen + lr.ll + 1);
			memcpy(txt + tlen, lr.buf, lr.ll);
			txt[tlen + lr.ll] = 0x0a;
			if(lr.ll == 0 && tlen > 0)
			  txtoff = tlen - 1;
			tlen += lr.ll + 1;
		}

		if(txt != NULL && pstart != -1 && pstop != -1 && got_ts)
		{
			txt[txtoff] = 0;
			if(txtoff>40 && strchr(txt, 0x0a) == NULL)
			{
				for(int cp=(int)(txtoff/2); cp<txtoff; cp++)
				{
					if(txt[cp] == 0x20)
					{
						txt[cp] = 0x0a;
						break;
					}
				}
			}
			StrToSentence(txt);
			//TRACE(TRACE_ERROR, "HLS", "S: %lli | E: %lli | T: %s", pstart+delay, pstop+delay, txt);
			vo = es_insert_text(es, txt, pstart+delay, pstop+delay, tag_flags);
			if(vo != NULL)
				video_overlay_enqueue(mp, vo);
		}

		got_ts = 0;

		if(txt != NULL) free(txt);
		if(lines>10)
		{
			//TRACE(TRACE_ERROR, "HLS", "Lines: %i", lines);
			pstart = pstop = -1;
			got_ts = 0;
		}
}

static char *
get_mpegts(char *v)
{
  char *value = v;
  if(value == NULL)
    return NULL;

  while((*value < '0' || *value > '9') && *value)
    value++;

  if(*value >= '0' && *value <= '9') {
    v = value;
    while(*v && *v != 10 && *v != 13  && *v >= '0' && *v <= '9')
      v++;
    if(*v)
      *v++ = 0;
  } else {
    v = value;
  }

  if(*v)
     *v++ = 0;

  return value;
}

#if 0 //USE_FMP4
static int
probe_non_muxed_subtitle_fmp4(ts_demuxer_t *td, const uint8_t *data, int size,
                hls_segment_t *hs)
{
	if(size < 30) return 0;
	//hexdump("BUFFER", data, MIN(size, 256)); return 0;

	if(!memcmp(data, "WEBVTT", 6))
	{

		media_pipe_t *mp = td->td_mp;
		if(mp->mp_subtitle.mq_stream2 == -1)
		{
			TRACE(TRACE_ERROR, "HLS", "MQ STREAM: %i - SKIP", mp->mp_subtitle.mq_stream2);
			//te->te_pts = PTS_UNSET;
			//te->te_dts = PTS_UNSET;
			//return 0;
		}

		//TRACE(TRACE_ERROR, "HLS", "MQ STREAM: %i - OK", mp->mp_subtitle.mq_stream2);

		int64_t delay = 0;

		parse_vtt((const char*)data, size, delay, mp);

		return 0;
	}


// bad:
  TRACE(TRACE_ERROR, "HLS", "Unable to probe subtitle contents");
  hexdump("BUFFER", data, MIN(size, 256));
  return -1;
}
#endif

/**
 *
 */
static int
probe_non_muxed_subtitle(ts_demuxer_t *td, const uint8_t *data, int size,
                hls_segment_t *hs)
{
	if(size < 30) return 0;
	//hexdump("BUFFER", data, MIN(size, 256)); return 0;

	if(!memcmp(data, "WEBVTT", 6))
	{

		media_pipe_t *mp = td->td_mp;
		if(mp->mp_subtitle.mq_stream2 == -1)
		{
			//TRACE(TRACE_ERROR, "HLS", "MQ STREAM: %i - SKIP", mp->mp_subtitle.mq_stream2);
			//te->te_pts = PTS_UNSET;
			//te->te_dts = PTS_UNSET;
			return 0;
		}

		hls_variant_t *hv = hs->hs_variant;
		assert(hv->hv_subtitle_stream != 0);

		ts_es_t *te = find_es(td, 0, 1);

		te->te_stream = hv->hv_subtitle_stream;
		te->te_codec = NULL;

		//hexdump("BUFFER", data, MIN(size, 256));

		te = LIST_FIRST(&td->td_elemtary_streams);
		assert(te != NULL);
		te->te_current_seq = hs->hs_seq;

		//TRACE(TRACE_ERROR, "HLS", "MQ STREAM: %i - OK", mp->mp_subtitle.mq_stream2);

		char *v;
		char *s = (char *)find_str((const char *)data, size, (const char*)"MPEGTS:");
		int64_t mpeg_ts_current = -1;
		int64_t delay = 0;

		if(s != NULL)
		{
			if((v = (char*)mystrbegins(s, "MPEGTS:")) != NULL) {
				//TRACE(TRACE_ERROR, "HLS", "V1: %s", v);
				//TRACE(TRACE_ERROR, "HLS", "V2: %s", get_mpegts(v));

				mpeg_ts_current = strtoull(get_mpegts(v), NULL, 10);
				delay = mpeg_ts_current*100/9;
				if(delay<=2000000) delay = -3500000;
				//TRACE(TRACE_ERROR, "HLS", "DELAY: %lli", delay);
			}
		}

		parse_vtt((const char*)data, size, delay, mp);

		te->te_pts = PTS_UNSET;
		te->te_dts = PTS_UNSET;
		return 0;

	}


// bad:
  TRACE(TRACE_ERROR, "HLS", "Unable to probe subtitle contents?");
  hexdump("BUFFER", data, MIN(size, 256));
  return -1;
}


#if USE_FMP4
/**
 *
 */
static int
probe_fmp4(ts_demuxer_t *td, /*uint8_t *data,*/ int size,
                hls_segment_t *hs, int type /* MB_AUDIO ... */, hls_demuxer_t *hd)
{
	//int o = 0;
	//hls_variant_t *hv = hs->hs_variant;
	uint8_t *data = td->td_buf_fmp4;

	int r = 0;

	//ts_es_t *te = LIST_FIRST(&td->td_elemtary_streams);
	ts_es_t *te = find_es(td, type==MB_VIDEO?0:1, 0);

	if(data == NULL) return -1;
	//if(te->h_fctx == NULL) return -1;

    te->te_data_type = type;
	te->te_current_seq = hs->hs_seq;

	int64_t current_offset = 0;
	double current_dts = 0.f; //base_pts;
	//const char *tfdt = "tfdt";
	const char *tfhd = "tfhd";
	const char *trun = "trun";
	const char *moof = "moof";
	const char *mdat = "mdat";
	//const char *sidx = "sidx";

// tfhd
#define Basedataoffsetpresent			0x000001
#define sampleDescriptionIndexPresent	0x000002
#define defaultSampleDurationPresent	0x000008
#define defaultSampleSizePresent		0x000010
#define defaultSampleFlagsPresent		0x000020

// trun
#define dataOffsetPresentFlag 			0x01
#define firstSampleFlagsPresentFlag		0x04
#define sampleDurationPresentFlag		0x100
#define sampleSizePresentFlag			0x200
#define sampleFlagsPresentFlag			0x400
#define sampleCTOPresentFlag			0x800

//float d_dur_scale;
int last_r = 0;
int additional_read = 0;

next_sidx:

	if(current_offset)
	{
		if(current_offset<last_r && current_offset<size)
		{
			data=td->td_buf_fmp4+current_offset;
			r = last_r - current_offset;
			//last_r -= current_offset;
		}
		else
		{
			return 0;
		}
		//TRACE(TRACE_DEBUG, "fMP4", "SKIP READ - off=%lli r=%i/%i", current_offset, r, size);
	}
	else
	{
		fa_seek(hs->hs_fh, 0, SEEK_SET);
		// bw_start = arch_get_avtime();
		int xr=0;
		while(1)
		{
			data = td->td_buf_fmp4;
			last_r = r = fa_read(hs->hs_fh, td->td_buf_fmp4+xr, td->td_buf_size-xr);
			if(r < 0) break;

			xr+=r;
			//TRACE(TRACE_INFO, "fMP4-DATA", "TD_MUX_MODE_UNSET-BUF: %i read, total=%i", r, xr);

			if(xr > td->td_buf_size-16356)
			{
				size = td->td_buf_size = xr + 4194280; //(xr+(4*1024*1024)-1) & -(1024*1024);
				td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, td->td_buf_size);
				//TRACE(TRACE_INFO, "fMP4-DATA", "td_buf_fmp4 REALLOC +4MB: %i bytes (%s)", td->td_buf_size, (te->te_data_type == MB_AUDIO?"A":"V"));
				data = td->td_buf_fmp4;
			}

			if(r == 0)
			{
				last_r = r = xr;
				break;
			}
		}

		if(xr > 0) add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, xr);

		//last_r = r = fa_read(hs->hs_fh, data, size); //8192

		/*if(r==size)
			additional_read = 1;
		else
			additional_read = 0;
		*/
		additional_read = 0;
	}
	if(r < 0) return -1; // EOF
	if( !r ) return 0; // NEXT SEGMENT

	//TRACE(TRACE_DEBUG, "fMP4", "off=%lli r=%i/%i", current_offset, r, size);

	//hexdump("BUFFER", data, 256);

	//const uint8_t *mp = (const uint8_t *)find_str((const char *)data, r, sidx); // sidx / muxed streams

	//TRACE(TRACE_ERROR, "probe_fmp4", "sidx present=%i", (mp!=NULL?1:0));
	const uint8_t *s[2];
	const uint8_t *d[2];
	const uint8_t *mo = (const uint8_t *)find_str((const char *)data, r, moof); // moof

	const uint8_t *md = (const uint8_t *)find_str((const char *)data, r, mdat); // mdat

	if(!mo || !md) return 0;

	d[0] = (const uint8_t *)find_str((const char *)data, r, tfhd); // default duration
	s[0] = (const uint8_t *)find_str((const char *)data, r, trun); // number of samples

#if 0 // muxed audio+video
	if(d[0] && s[0])
	{
		d[1] = (const uint8_t *)find_str((const char *)(d[0]+4), r, tfhd); // default duration
		s[1] = (const uint8_t *)find_str((const char *)(s[0]+4), r, trun); // number of samples
	}

	int idx = 0;
	if(s[1] != NULL && d[1] != NULL)
	{
		/*TRACE(TRACE_ERROR, "probe_fmp4", "trun1=%i", (s[0]-data));
		TRACE(TRACE_ERROR, "probe_fmp4", "trun2=%i", (s[1]-data));
		TRACE(TRACE_ERROR, "probe_fmp4", "tfhd1=%i", (d[0]-data));
		TRACE(TRACE_ERROR, "probe_fmp4", "tfhd2=%i", (d[1]-data));
		*/
		if(type == MB_AUDIO)
			idx = 1;
	}
#else
	int idx = 0;
#endif

#if 0
	if(te->te_data_type == MB_AUDIO)
	{
		d_dur_scale = (float)(1000000.f) / (float)(te->time_sr);
	}
	else
	if(te->te_data_type == MB_VIDEO)
	{
		if(te->time_num==1 && te->time_den>120)
		{
			d_dur_scale = (1000000.f / (float)te->time_den) * 2.f;
		}
		else
		{
			d_dur_scale = (1000.f / (float)((float)te->time_den / (float)te->time_num));
		}
	}
#endif

	if(/*t == NULL ||*/ s[idx] == NULL)
	{
		//if(t==NULL) TRACE(TRACE_ERROR, "probe_fmp4", "tfdt=NULL");
		//if(d==NULL) TRACE(TRACE_ERROR, "probe_fmp4", "mdat=NULL");
		//if(s==NULL) TRACE(TRACE_ERROR, "probe_fmp4", "trun=NULL");
		return -2; // BAD_VARIANT
	}

	int samples = rd32_be(s[idx]+8);
	//TRACE(TRACE_ERROR, "probe_fmp4", "type=%s samples=%i", type==MB_VIDEO?"V":"A", samples);
	int32_t sample_size[samples];
	int64_t sample_pts[samples];
	int64_t sample_dts[samples];

	int32_t dataoffset 	= current_offset + rd32_be(s[idx] + 12) + (mo - data) - 4;

	if(md)
		current_offset += rd32_be(md - 4) + (md - data) - 4;

	float default_dur = 0;//(type==MB_VIDEO?512.f:1024.f);//rd32_be(d[idx] + 16);
	int32_t default_siz = 0;
	//int32_t firstSampleFlags = 0;

	uint32_t flags = rd32_be(d[idx]+4);
	int flag_pos = 8;

	if(flags & Basedataoffsetpresent) flag_pos+=8;
	if(flags & sampleDescriptionIndexPresent) flag_pos+=8;
	if(flags & defaultSampleDurationPresent)
	{
		default_dur = rd32_be(d[idx] + flag_pos);
		flag_pos += 4;
		//TRACE(TRACE_INFO, "HLS", "default_dur=%.3f (defaultSampleDurationPresent)", default_dur);
	}
	if(flags & defaultSampleSizePresent)
	{
		default_siz = rd32_be(d[idx] + flag_pos);
		flag_pos += 4;
		//TRACE(TRACE_INFO, "HLS", "default_siz=%i (defaultSampleSizePresent) (%s)", default_siz, type==MB_VIDEO?"V":"A");
		if(default_dur && default_siz > default_dur)
		{
			default_dur = default_siz / default_dur;
			//TRACE(TRACE_ERROR, "HLS", "default_siz=%i default_dur=%.3f (%s)", default_siz, default_dur, type==MB_VIDEO?"V":"A");
		}
	}
//hexdump("BUFFER", data, 256);
	uint32_t flags_trun = rd32_be(s[idx]+4);
	int flag_trun_pos = 8+4; // #trun+flags+samples = 4+4+4

	if(flags_trun & dataOffsetPresentFlag)
	{
		//TRACE(TRACE_INFO, "HLS", "trun: data_offset=%i (we have %i)", rd32_be(s[idx]+flag_trun_pos), dataoffset);
		flag_trun_pos += 4;
	}
	if(flags_trun & firstSampleFlagsPresentFlag)
	{
		//firstSampleFlags = rd32_be(s[idx]+flag_trun_pos);
		//TRACE(TRACE_INFO, "HLS", "trun: firstSampleFlags");
		flag_trun_pos += 4;
	}

	//default_dur = rd32_be(d[idx] + 16);
	//default_dur = 1536; if(type==MB_VIDEO) default_dur = 1024;
	uint32_t sample_size_total = 0;

	//if(type == MB_VIDEO) hexdump("BUFFER", s, 2048);

	//int32_t sample_ts = rd32_be(s[idx]+4) & 0x0800;  // 0000 1010 0000 0101
	//if(mp!=NULL && type==MB_VIDEO) sample_ts = 1;

	int s_dur = 0;
	int s_siz = 0;
	int32_t s_off = 0;
	//float coef = 0.f;
	//TRACE(TRACE_INFO, "HLS", "te->te_coef=%.2f", te->te_coef);

	for(int i=0; i<samples; i++)
	{
		s_off = 0;

		if(flags_trun & sampleDurationPresentFlag)
		{
			default_dur = s_dur = rd32_be(s[idx]+flag_trun_pos);
			//TRACE(TRACE_INFO, "HLS", "trun: sampleDurationPresentFlag s_dur=%i", s_dur);
			flag_trun_pos += 4;
		}

		if(flags_trun & sampleSizePresentFlag)
		{
			s_siz = rd32_be(s[idx]+flag_trun_pos);
			//TRACE(TRACE_INFO, "HLS", "trun: sampleSizePresentFlag s_siz=%i", s_siz);
			flag_trun_pos += 4;
		}

		if(flags_trun & sampleFlagsPresentFlag)
			flag_trun_pos += 4;

		if(flags_trun & sampleCTOPresentFlag)
		{
			s_off = rd32_be(s[idx]+flag_trun_pos);
			flag_trun_pos += 4;
			//TRACE(TRACE_ERROR, "HLS", "trun: sampleCTOPresentFlag s_off=%i %s", s_off, (type==MB_VIDEO?"V":"A"));
		}


		if(!s_siz)
		{
			s_siz = default_siz;
			//TRACE(TRACE_ERROR, "HLS", "trun: sampleSizePresentFlag s_siz=%i (default)", s_siz);
		}

		if(!s_dur)
		{
			//TRACE(TRACE_INFO, "HLS", "trun: sampleDurationPresentFlag s_dur=%i (default=%.3f) (%s)", s_dur, default_dur, (type==MB_VIDEO?"V":"A"));
			if(!default_dur)
			{
				if(type==MB_VIDEO)
				{
					if(te->time_num == te->time_div)
						default_dur = s_dur = 1000;
					else
						default_dur = s_dur = te->time_div;
				}
				else
				{
					default_dur = s_dur = (te->time_sr==44100?588*2:512*2);
					//TRACE(TRACE_ERROR, "HLS", "%i/%i trun: sampleDurationPresentFlag s_dur=%i (default) (%s)", i+1, samples, s_dur, (type==MB_VIDEO?"V":"A"));
				}
			}
		}

		sample_size[i] = s_siz;
		//if(current_dts + s_off < 1000) TRACE(TRACE_ERROR, "HLS", "dts: %.3f s_off=%i", current_dts, s_off);
		sample_pts[i] = current_dts + s_off;
		sample_dts[i] = current_dts;

		current_dts += default_dur;

		if(te->te_data_type == MB_AUDIO)
		{
			if(!(te->time_sr)) te->time_sr = 48000;

			//TRACE(TRACE_DEBUG, "HLS", "%.3f * %lli * 1000 / %i", default_dur, sample_pts[i], te->time_sr);
			//coef = (float)(default_dur * 1000.f) / (float)(te->time_sr);
			//int64_t tt = (coef * 1000.f * (float)sample_pts[i])/1000;
			//TRACE(TRACE_ERROR, "HLS", "IN :   s_off=%i  dts: %lli  pts: %lli  default_dur: %.3f  te->time_sr: %i  te->time_bas: %i  te->time_div: %i", s_off, sample_dts[i], sample_pts[i], default_dur, te->time_sr, te->time_bas, te->time_div);
			sample_pts[i] = (1000000 * sample_pts[i]) / te->time_sr;
			sample_dts[i] = (1000000 * sample_dts[i]) / te->time_sr;
			//TRACE(TRACE_ERROR, "HLS", "OUT:   s_off=%i  dts: %lli  pts: %lli", s_off, sample_dts[i], sample_pts[i]);

			//TRACE(TRACE_DEBUG, "HLS", "tt=%lli rr=%lli dd=%lli", tt, sample_pts[i], sample_pts[i]-tt);

			//TRACE(TRACE_DEBUG, "HLS", "MB_AUDIO %03i/%03i: DUR: %.3f PTS: %lli", i+1, samples, default_dur, sample_pts[i]);
		}
		else
		if(te->te_data_type == MB_VIDEO)
		{
			//TRACE(TRACE_DEBUG, "HLS", "%lli * %i * 1000000 / %i / %.3f", sample_dts[i], te->time_num, te->time_den, default_dur);
			if(te->time_ok)
			{
				//coef = ((1000000.f * (float)(float)te->time_div) / (float)((float)te->time_bas)) / (float)default_dur;
				//int64_t tt = (coef * 1000.f * (float)sample_pts[i])/1000;

				sample_pts[i] = ((sample_pts[i] * 1000000 * te->time_div) / te->time_bas) / default_dur;
				sample_dts[i] = ((sample_dts[i] * 1000000 * te->time_div) / te->time_bas) / default_dur;

				//TRACE(TRACE_DEBUG, "HLS-01", "tt=%lli rr=%lli dd=%lli", tt, sample_pts[i], sample_pts[i]-tt);

				//sample_pts[i] = (coef * 1000.f * (float)sample_pts[i])/1000;
				//sample_dts[i] = (coef * 1000.f * (float)sample_dts[i])/1000;
			}
			else
			{
				if(te->time_num==1 && te->time_bas /*te->time_den*/>120)
				{
					int tb = 1;
					int td = 1000000;

					if(te->time_bas % 1024 == 0) tb = 1024;
					else if(te->time_bas % 512 == 0) tb = 512;
					else if(te->time_bas>25000)	td *= 2;

					/*float coef = 0.f;
					if((float)te->time_bas/1024.f == te->time_bas/1024) coef = 1000000.f / (te->time_bas/1024);
					else if((float)te->time_bas/512.f == te->time_bas/512) coef = 1000000.f / (te->time_bas/512);
					else
					{
						coef = (1000000.f / (float)te->time_bas); // te->time_den
						//if(te->time_den>25000)
						if(te->time_bas>25000)
							coef *= 2.f;
						//TRACE(TRACE_ERROR, "HLS", "!!! MB_VIDEO: DUR: %.3f COEF: %.3f", default_dur, coef);
					}
					//TRACE(TRACE_INFO, "HLS", "!!! MB_VIDEO: DUR: %.3f COEF: %.3f", default_dur, coef);
					int64_t tt = (coef * 1000.f * (float)sample_pts[i])/1000;
					*/

					sample_pts[i] = (td * sample_pts[i] * tb) / te->time_bas;
					sample_dts[i] = (td * sample_dts[i] * tb) / te->time_bas;

					//TRACE(TRACE_DEBUG, "HLS02", "tt=%lli rr=%lli dd=%lli", tt, sample_pts[i], sample_pts[i]-tt);

					/*
					float coef = 0.f;
					if((float)te->time_bas/1024.f == te->time_bas/1024) coef = 1000000.f / (te->time_bas/1024);
					else if((float)te->time_bas/512.f == te->time_bas/512) coef = 1000000.f / (te->time_bas/512);
					else
					{
						coef = (1000000.f / (float)te->time_bas); // te->time_den
						//if(te->time_den>25000)
						if(te->time_bas>25000)
							coef *= 2.f;
						//TRACE(TRACE_ERROR, "HLS", "!!! MB_VIDEO: DUR: %.3f COEF: %.3f", default_dur, coef);
					}
					//TRACE(TRACE_INFO, "HLS", "!!! MB_VIDEO: DUR: %.3f COEF: %.3f", default_dur, coef);

					TRACE(TRACE_DEBUG, "HLS02", "tt=%lli rr=%lli dd=%lli", tt, sample_pts[i], sample_pts[i]-tt);

					sample_pts[i] = (coef * 1000.f * (float)sample_pts[i])/1000;
					sample_dts[i] = (coef * 1000.f * (float)sample_dts[i])/1000;
					*/

				}
				else
				{
					//coef = ((1000000.f * (float)te->time_num) / (float)((float)te->time_bas/*te->time_den*/)) / (float)default_dur;
					//int64_t tt = (coef * 1000.f * (float)sample_pts[i])/1000;

					sample_pts[i] = ((sample_pts[i] * 1000000 * te->time_num) / te->time_bas) / default_dur;
					sample_dts[i] = ((sample_dts[i] * 1000000 * te->time_num) / te->time_bas) / default_dur;

					//TRACE(TRACE_DEBUG, "HLS03", "tt=%lli rr=%lli dd=%lli", tt, sample_pts[i], sample_pts[i]-tt);

					//TRACE(TRACE_INFO, "HLS", "--- MB_VIDEO: DUR: %.3f COEF: %.3f", default_dur, coef);
				}
			}
			//TRACE(TRACE_DEBUG, "HLS", "MB_VIDEO: DUR: %.3f PTS: %lli", default_dur, sample_pts[i]);
		}

		//TRACE(TRACE_INFO, "HLS", "PTS MB: DUR: %.3f COEF: %.3f PTS: %lli", default_dur, coef, sample_pts[i]);

		sample_size_total += sample_size[i];
	}

	/*
	media_codec_t *mc = te->te_codec;

	if(mc == NULL)
	{
		TRACE(TRACE_INFO, "probe_fmp4", "mc == NULL");
		return -1;
	}
	*/

	if(sample_size_total>size)
	{
		size = (sample_size_total+4194280); //(4*1024*1024)-1) & -(1024*1024);
		td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, size);
		data = td->td_buf_fmp4;
		td->td_buf_size = size;
		additional_read = 1;
		//TRACE(TRACE_INFO, "probe_fmp4", "sample buffer re-allocated: size=%i (%s)", size, (te->te_data_type == MB_AUDIO?"A":"V"));
	}

	//memset(data, 0, size);

	if(additional_read)
	{
		//TRACE(TRACE_ERROR, "probe_V", "additional_read");
		data = td->td_buf_fmp4;
		fa_seek(hs->hs_fh, dataoffset, SEEK_SET);
		// bw_start = arch_get_avtime();
		r = fa_read(hs->hs_fh, data, MAX(td->td_buf_size, sample_size_total));
		if(r <= 0) return 0;

		add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, r);

	}
	else
		data=td->td_buf_fmp4+dataoffset;

	r = 0;

	if(cancellable_is_cancelled(hd->hd_cancellable)) return -1;

	//TRACE(TRACE_INFO, "probe_fmp4", "hs->hs_duration=%lli", hs->hs_duration);

	if(samples>1) te->te_duration = sample_dts[1] - sample_dts[0];

	if(type==MB_VIDEO)
	{
		int64_t keyframe_cnt = 0;
		int keyframe = 1;

		for(int i=0; i<samples; i++)
		{
			te->te_dts = sample_dts[i];// + hs->hs_duration;
			te->te_pts = sample_pts[i];// + hs->hs_duration;
			//if(te->te_pts == te->te_dts) keyframe |= 1;
			//if(keyframe) {TRACE(TRACE_DEBUG, "HLS", "KEY:%i sample=%i/%i", keyframe, i+1, samples);}
			//if(te->te_data_type == MB_AUDIO && i && te->te_pts != te->te_dts) {te->te_pts = te->te_dts = AV_NOPTS_VALUE; keyframe = 0;}
			parse_data_fmp4(td, hs->hs_variant, te, data+r, sample_size[i], keyframe); // !i || !(i%24));

			//if(te->te_codec->codec_id == AV_CODEC_ID_AV1) keyframe = 1;
			//if(keyframe) TRACE(TRACE_INFO, "probe_fmp4", "keyframe %i/%i = %i (pos: %lli, dur: %i)", i+1, samples, keyframe, keyframe_cnt, te->te_duration);
			r+=sample_size[i];
			keyframe_cnt += te->te_duration;

			if(keyframe_cnt-te->te_duration > 10000000 || (keyframe_cnt-te->te_duration > 5000000 && sample_dts[i]==sample_pts[i]))
			{
				keyframe_cnt = 0;
				keyframe = 2;
			}
			else keyframe = 0;
		}
	}
#if 1 // 1 sample
	else
	if(type==MB_AUDIO)
	{
		for(int i=0; i<samples; i++)
		{
			//TRACE(TRACE_INFO, "probe_fmp4", "sample %i/%i", i+1, samples);

			te->te_dts = sample_dts[i];
			te->te_pts = sample_pts[i];

			parse_data_fmp4(td, hs->hs_variant, te, data+r, sample_size[i], 1); //!i || !(i%35));
			r+=sample_size[i];
		}
	}

#else	// 2 samples + 3
	else
	if(type==MB_AUDIO)
	{
		for(int i=0; i<samples-1; i++)
		{
			//TRACE(TRACE_INFO, "probe_fmp4", "sample %i/%i", i+1, samples);

			te->te_dts = sample_dts[i];
			te->te_pts = sample_pts[i];

			parse_data_fmp4(td, hs->hs_variant, te, data+r, sample_size[i]+sample_size[i+1], 1); //!i || !(i%35));

			r+=sample_size[i];
			i++;
			r+=sample_size[i];

			if(i==samples-4)
			{
				i++;
				te->te_dts = sample_dts[i];
				te->te_pts = sample_pts[i];
				//TRACE(TRACE_ERROR, "probe_fmp4", "sample %i/%i (last 3)", i+1, samples);
				parse_data_fmp4(td, hs->hs_variant, te, data+r, sample_size[i]+sample_size[i+1]+sample_size[i+2], 1);
				//parse_data_fmp4(td, hs->hs_variant, te, data+r, sample_size[i], 1);

				break;
			}

			//hexdump("BUFFER", data, 64);
		}
	}
#endif
	if(cancellable_is_cancelled(hd->hd_cancellable)) return -1;

	goto next_sidx;

	return 0;
}
#endif

/**
 *
 */
static int
probe_non_muxed_audio(ts_demuxer_t *td, const uint8_t *data, int size,
                hls_segment_t *hs)
{
  int64_t ptsoffset = 0;
  int o = 0;
  hls_variant_t *hv = hs->hs_variant;

  // Not really an ID3 parser, we just search more or less blindly
  //if(!memcmp(data, "ID3", 3))
  {
    const char *s = "com.apple.streaming.transportStreamTimestamp";
    const uint8_t *t = (const uint8_t *)find_str((const char *)data, size, s);
    if(t != NULL) {
      int off = t - data + 1;
      if(off <= size - 8) {
        ptsoffset = rd64_be(t + strlen(s) + 1);
        o = off + strlen(s) + 8;
      }
    }
  }

  //if(o + 7 >= 188 || o > size)
  //  goto bad;

	while(1)
	{
		if(o + 9 >= size)
		    goto bad;
		if(!memcmp(data + o, "ID3", 3))
		{
			o = o + 9 + data[o + 9] + 1;
			if(o + 9 >= size)
				goto bad;
		}
		else
			break;

	}

  uint32_t h1 = rd32_be(data + o);

  if((h1 & 0xfff60000) == 0xfff00000) {
    // AAC
    ts_es_t *te = find_es(td, 0, 1);

    if(te->te_codec != NULL && te->te_codec->codec_id != AV_CODEC_ID_AAC) {
      media_codec_deref(te->te_codec);
      te->te_codec = NULL;
    }

    if(te->te_codec == NULL) {
	  td->td_mp->mp_hls_source |= 3;
      te->te_codec = media_codec_create(AV_CODEC_ID_AAC, 1, NULL, NULL, NULL,
                                        td->td_mp);
      assert(hv->hv_audio_stream != 0);
      te->te_stream = hv->hv_audio_stream;
	  //TRACE(TRACE_ERROR, "HLS", "Unmuxed AAC");

    }

    te->te_data_type = MB_AUDIO;
    te->te_bts = te->te_dts = te->te_pts = ptsoffset;
    te->te_probe_frame = 1;
    unmuxed_input(td, data + o, size - o, hs);
	//TRACE(TRACE_DEBUG, "fMP4-DATA", "unmuxed_input: %i bytes (%lli sec)", size - o, te->te_bts/90000);
    return 0;
  }

	if((h1 & 0xffff0000) == 0x0b770000)
	{
		uint32_t h2 = (rd32_be(data + o + 4) & 0x00f80000) >> 19;
		//TRACE(TRACE_ERROR, "HLS", "0x0b770000 - %u", h2);
		//if((h2 & 0x00f80000) == 0x00800000)
		if(h2>=11 && h2<=16)
		{
			// https://www.atsc.org/wp-content/uploads/2015/03/A52-201212-17.pdf page 162
			// E-AC3 0B 77 CR CR HZ (10000 XXX
			ts_es_t *te = find_es(td, 0, 1);

			if(te->te_codec != NULL && te->te_codec->codec_id != AV_CODEC_ID_EAC3) {
			  media_codec_deref(te->te_codec);
			  te->te_codec = NULL;
			}

			if(te->te_codec == NULL) {
			  td->td_mp->mp_hls_source |= 3;
			  te->te_codec = media_codec_create(AV_CODEC_ID_EAC3, 1, NULL, NULL, NULL,
												td->td_mp);
			  assert(hv->hv_audio_stream != 0);
			  te->te_stream = hv->hv_audio_stream;
			  //TRACE(TRACE_ERROR, "HLS", "Unmuxed EAC3");
			}

			te->te_data_type = MB_AUDIO;
			te->te_bts = te->te_dts = te->te_pts = ptsoffset;
			te->te_probe_frame = 1;
			unmuxed_input(td, data + o, size - o, hs);
			//TRACE(TRACE_DEBUG, "fMP4-DATA", "unmuxed_input: %i bytes (%lli sec)", size - o, te->te_bts/90000);
			return 0;
		}
		else
		{
			// AC3 0B 77 15 41
			ts_es_t *te = find_es(td, 0, 1);

			if(te->te_codec != NULL && te->te_codec->codec_id != AV_CODEC_ID_AC3) {
			  media_codec_deref(te->te_codec);
			  te->te_codec = NULL;
			}

			if(te->te_codec == NULL) {
			  td->td_mp->mp_hls_source |= 3;
			  te->te_codec = media_codec_create(AV_CODEC_ID_AC3, 1, NULL, NULL, NULL,
												td->td_mp);
			  assert(hv->hv_audio_stream != 0);
			  te->te_stream = hv->hv_audio_stream;
			  //TRACE(TRACE_ERROR, "HLS", "Unmuxed AC3");
			}

			te->te_data_type = MB_AUDIO;
			te->te_bts = te->te_dts = te->te_pts = ptsoffset;
			te->te_probe_frame = 1;
			unmuxed_input(td, data + o, size - o, hs);
			//TRACE(TRACE_DEBUG, "fMP4-DATA", "unmuxed_input: %i bytes (%lli sec)", size - o, te->te_bts/90000);
			return 0;
		}
	}

 bad:
  TRACE(TRACE_ERROR, "HLS", "Unable to probe audio contents");
  hexdump("BUFFER", data, MIN(size, 256));
  return -1;
}


/**
 *
 */
media_buf_t *
hls_ts_demuxer_read(hls_demuxer_t *hd)
{
	const hls_t *h = hd->hd_hls;
	media_pipe_t *mp = h->h_mp;

	static int need_rewind;

	if(hd->hd_current == NULL)
		return HLS_EOF;

	while(1)
	{

    assert(hd->hd_current != NULL);

    //hls_check_bw_switch(hd);
	//hls_bw_switch(hd, 3000000);

    if(hd->hd_req != NULL) {

      HLS_TRACE(h, "Switching from %s to %s",
                hd->hd_current->hv_name, hd->hd_req->hv_name);
      hls_variant_close(hd->hd_current);

      hd->hd_current = hd->hd_req;
      hd->hd_req = NULL;

      if(hd == &h->h_primary) {
        // Primary demuxer always control the video queue
        mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
        mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;

        if(h->h_audio.hd_current == NULL) {
          // If no audio variant is running we are also controlling
          // the audio queue
          mp->mp_audio.mq_demuxer_flags |= HLS_QUEUE_MERGE;
          mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
        }
      }

      if(hd == &h->h_audio) {
        // If the audio demuxer is switching we by definition
        // control the audio queue
        mp->mp_audio.mq_demuxer_flags |= HLS_QUEUE_MERGE;
        mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
      }
    }


    hls_variant_t *hv = hd->hd_current;
    ts_demuxer_t *td = hv->hv_demuxer_private;

    if(td == NULL) {
      assert(hv->hv_demuxer_private == NULL);
      td = hv->hv_demuxer_private = calloc(1, sizeof(ts_demuxer_t));
      TAILQ_INIT(&td->td_packets);
      assert(hv->hv_demuxer_close == NULL);
      hv->hv_demuxer_close = hls_ts_demuxer_close;
      hv->hv_demuxer_flush = hls_ts_demuxer_flush;
      td->td_mp = mp;
      td->td_hd = hd;
    }

    if(hd->hd_seek_to_segment != PTS_UNSET && hv->hv_current_seg != NULL) {
      hls_segment_close(hv->hv_current_seg);
      hv->hv_current_seg = NULL;
    }

	if(hv->hv_current_seg != NULL)
	{
		media_buf_t *mb = get_pkt(td);

		if(mb != NULL)
		  return mb;
	}

    int attempts = 0;

    if(hv->hv_current_seg == NULL || hv->hv_current_seg->hs_fh == NULL) {

      hls_segment_t *hs;
      while(1) {

        hs = hls_variant_select_next_segment(hv);

        if(hs == HLS_NYA)
		{
			continue;
		}

        if(hs == HLS_EOF)
          return HLS_EOF;

         if(hs == NULL)
		 {
          return NULL;
		 }

        hls_variant_open(hv);

        HLS_TRACE(h, "%s: Opening variant %s sequence %d discontinuity-seq:%d",
                  hd->hd_type, hv->hv_name, hs->hs_seq,
                  hs->hs_discontinuity_segment->hds_seq);

        break;
      }

      while(1) {

        //assert(hs != NULL);
        hls_error_t err = hls_segment_open(hs);

        if(cancellable_is_cancelled(hd->hd_cancellable)) {
          hls_segment_close(hs);
          hv->hv_current_seg = NULL;
          return NULL;
        }

        if(err)// /*!hv->hv_frozen && */ err == HLS_ERROR_SEGMENT_NOT_FOUND)
		{
          /*
           * When in live mode segments may disappear and with a bit
           * of unluck that can happen between when we load the
           * playlist and try to open the segment, so retry a few times
           */
          if(attempts < 10)
		  {
            attempts++;
			if(hs != NULL)
				hls_segment_close(hs);
			hv->hv_current_seg = NULL;
			if(attempts > 1)
			{
				hs = TAILQ_NEXT(hs, hs_link);
			}

			if(hs != NULL)
				continue;
          }
        }

        if(err)
		{
			if(hs != NULL)
				hls_segment_close(hs);
			hv->hv_current_seg = NULL;

			hls_bad_variant(hv, err);

			TRACE(TRACE_ERROR, "HLS", "No segments available (last error #%i - %s)", err, hlserrstr[err]);

			/* if(hd->hd_req != NULL)
			{
				TRACE(TRACE_DEBUG, "HLS", "Switching variants");
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
				return NULL;
			} */

			return HLS_EOF; //NULL;
        }
        hv->hv_current_seg = hs;
#if USE_FMP4
		if(hs->hs_map != HLS_MAP_FMP4)
			td->td_mux_mode = TD_MUX_MODE_UNSET;
#else
		if(hs->hs_map == HLS_MAP_FMP4)
		{
			TRACE(TRACE_ERROR, "HLS", "fMP4 format is not supported");
			return HLS_EOF;
		}
		td->td_mux_mode = TD_MUX_MODE_UNSET;
#endif

        break;
      }
    }

	attempts = 0;

    // Ok, try to read some bytes

    hls_segment_t *hs = hv->hv_current_seg;
    int r;
	int xr=0;
    int hds = hs->hs_discontinuity_segment->hds_seq;

    if(hds != hd->hd_discontinuity_seq)
	{
		if(hs->hs_map != HLS_MAP_FMP4)
			drain_parsers(td);

		hd->hd_discontinuity_seq = hds;
		//hd->hd_discontinuity = 1;
    }

#if 0
    if(hd->hd_discontinuity) {

      return HLS_DIS;
    }
#endif

#if USE_FMP4
	if(hs->hs_map == HLS_MAP_FMP4)
	{
		//td->td_mp->mp_audio_created = 3;
		if(td->td_mux_mode != TD_MUX_MODE_FMP4)
		{
			td->td_mux_mode = TD_MUX_MODE_FMP4;
			if(hd == &h->h_primary)
			{
				ts_es_t *te = find_es(td, 0, 1);
#if 0
				if(td->td_hd->hd_hls->h_codec_h264 != NULL)
				{
					TRACE(TRACE_ERROR, "fMP4-V", "media_codec_deref");
					//te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
					//media_codec_deref(td->td_hd->hd_hls->h_codec_h264);
					//usleep(1000000);
					td->td_hd->hd_hls->h_codec_h264 = NULL;
					//continue;
					/*
					te->time_bas = td->td_hd->hd_hls->time_bas;
					te->time_div = td->td_hd->hd_hls->time_div;
					te->time_num = td->td_hd->hd_hls->time_num;
					te->time_den = td->td_hd->hd_hls->time_den;
					te->time_ok  = td->td_hd->hd_hls->time_ok;
					goto set_te_video;
					*/
				}
				/*else
				if(te->te_codec != NULL)
				{
					TRACE(TRACE_DEBUG, "HLS", "Codec ref");
					te->te_codec = media_codec_ref(te->te_codec);
					te->time_bas = td->td_hd->hd_hls->time_bas;
					te->time_div = td->td_hd->hd_hls->time_div;
					te->time_num = td->td_hd->hd_hls->time_num;
					te->time_den = td->td_hd->hd_hls->time_den;
					te->time_ok  = td->td_hd->hd_hls->time_ok;
					goto set_te_video;
				}*/
				else
#endif
				{

//if(te->h_fctx != NULL) fa_libav_close_format(te->h_fctx, 0);

te->h_fmt = av_find_input_format("mp4");
//if(!te->h_fmt) { TRACE(TRACE_ERROR, "fMP4", "av_find_input_format"); return HLS_EOF;}

int flags = 0;//(FA_BUFFERED_NO_PREFETCH | FA_BUFFERED_SMALL);
//int flags = (FA_BUFFERED_NO_PREFETCH | FA_BUFFERED_SMALL);

if(strcmp(hs->hs_url, rstr_get(hs->hs_map_url)))
	flags |= FA_NO_PARKING;

for(int t=0;t<10;t++)
{
	te->h_fh = fa_open_ex(rstr_get(hs->hs_map_url), NULL, 0, flags, NULL);
	if(te->h_fh) break;
	usleep(500000);
}
if(!te->h_fh) { /*TRACE(TRACE_ERROR, "fMP4", "Unable to open video MAP file");*/ return HLS_EOF;}

te->h_avio = fa_libav_reopen(te->h_fh, 1);
if(!te->h_avio) { /*TRACE(TRACE_ERROR, "fMP4", "Video MAP AVIO failed");*/ return HLS_EOF;}
//te->h_avio->direct = 1;
//TRACE(TRACE_DEBUG, "fMP4-MAP-V", "%s: Probed as %s", rstr_get(hs->hs_map_url), te->h_fmt->name);

te->h_fctx = avformat_alloc_context();
//if(!te->h_fctx) { TRACE(TRACE_ERROR, "fMP4", "avformat_alloc_context"); return HLS_EOF;}

te->h_fctx->pb = te->h_avio;
te->h_fctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

//te->h_fctx->flags = AVFMT_FLAG_FLUSH_PACKETS;
media_codec_params_t mcp = {0};

if(hd->hd_current && hd->hd_current->hv_width && hd->hd_current->hv_height)
{
	mcp.width=hd->hd_current->hv_width;
	mcp.height=hd->hd_current->hv_height;
}

if(hd->hd_current && hd->hd_current->hv_fps)
{
	mcp.frame_rate_num = ((float) hd->hd_current->hv_fps * 10000.f);
	mcp.frame_rate_den = 10000;
}

if(!avformat_open_input(&te->h_fctx, rstr_get(hs->hs_map_url), te->h_fmt, NULL))
{
	te->h_fctx->fps_probe_size = 60;
    te->h_fctx->max_analyze_duration = 1000000;//1000000;
	te->h_fctx->probesize = 65536;
	//td->td_muxed_audio = 0;
	if(!avformat_find_stream_info(te->h_fctx, NULL))
	{
		for (int i = 0; i < te->h_fctx->nb_streams; i++)
		{
			if(te->h_fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				TRACE(TRACE_ERROR, "fMP4-MAP-V", "Stream contains muxed audio (not supported)");
				//td->td_muxed_audio = 1;
				continue;
			}
			if(te->h_fctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO) continue;
			te->h_ctx = te->h_fctx->streams[i]->codec;
			char str[256];

			avcodec_string(str, sizeof(str), te->h_ctx, 0);
//			TRACE(TRACE_DEBUG, "fMP4", " Stream #%d: %s", i, str);
			TRACE(TRACE_INFO, "fMP4-MAP-V", "%s", str);

			//TRACE(TRACE_INFO, "fMP4-MAP-V", "w=%i h=%i", te->h_fctx->streams[i]->codec->width, te->h_fctx->streams[i]->codec->height);

			te->time_den = te->h_fctx->streams[i]->codec->time_base.den;
			te->time_num = te->h_fctx->streams[i]->codec->time_base.num;

			if(te->h_fctx->streams[i]->codec->width && te->h_fctx->streams[i]->codec->height)
			{
				mcp.width = te->h_fctx->streams[i]->codec->width;
				mcp.height = te->h_fctx->streams[i]->codec->height;
			}
			if(te->h_fctx->streams[i]->codec->framerate.num)
			{
				mcp.frame_rate_num = te->h_fctx->streams[i]->codec->framerate.num;
				mcp.frame_rate_den = te->h_fctx->streams[i]->codec->framerate.den;
				TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video map info: %ix%i %.3f fps (%i/%i)", mcp.width, mcp.height, (float)mcp.frame_rate_num/(float)mcp.frame_rate_den, te->h_fctx->streams[i]->codec->framerate.num, te->h_fctx->streams[i]->codec->framerate.den);

				te->time_bas = te->h_fctx->streams[i]->codec->framerate.num;
				te->time_div = te->h_fctx->streams[i]->codec->framerate.den;
				te->time_ok = 1;
			}
			else
			{
				te->time_ok = 0;
				//if(te->h_fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
				//	TRACE(TRACE_DEBUG, "probe", "%i VIDEO", i);
				//if(te->h_fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
				//	TRACE(TRACE_DEBUG, "probe", "%i AUDIO", i);

				//TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Stream time base: %i/%i", te->h_fctx->streams[i]->codec->time_base.den, te->h_fctx->streams[i]->codec->time_base.num);
				//TRACE(TRACE_DEBUG, "fMP4-MAP", "Stream delay=%i", te->h_ctx->delay);

				te->time_bas = te->h_fctx->streams[i]->codec->time_base.den;
				te->time_div = te->h_fctx->streams[i]->codec->time_base.num;

				int current_fps = (int)(hd->hd_current->hv_fps*1000.f);

				if(te->time_div==1 && te->time_bas>120 && strstr(str, "hev1"))
				{
					te->time_div = 1000;
						 if(te->time_bas==120000 && current_fps == 59940) {te->time_div = 2002;}
					else if(te->time_bas==60000 && current_fps == 59940) {te->time_div = 1001;}
					else if(te->time_bas==60000 && current_fps == 29970) {te->time_div = 2002;}
					else if(te->time_bas==100000 && current_fps == 50000) {te->time_div = 2000;}
					else if(te->time_bas==50000 && current_fps == 25000) {te->time_div = 2000;}
					else if(te->time_bas==48000 && current_fps == 23976) {te->time_div = 2002;}
					else if(te->time_bas==30000 && current_fps == 29970) {te->time_div = 1001;}
					else if(te->time_bas==24000 && current_fps == 23976) {te->time_div = 1001;}
					else if(te->time_bas==100000) te->time_div = 2000;

					te->time_ok = 1;
					TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video map info: %ix%i %.3f fps (%i/%i)", mcp.width, mcp.height, (float)mcp.frame_rate_num/(float)mcp.frame_rate_den, te->time_bas, te->time_div);
				}

				//fa_seek(te->h_fh, 0, SEEK_SET);
				// bw_start = arch_get_avtime();
				uint8_t *data = calloc(1, 128*1024);
				int r = fa_read(te->h_fh, data, 128*1024);

				if(r > 0) add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, r);

				if(r < 100)
				{
					fa_seek(te->h_fh, 0, SEEK_SET);
					r = fa_read(te->h_fh, data, 128*1024);
				}
				const uint8_t *bas = (const uint8_t *)find_str((const char *)data, r, "sidx");
				const uint8_t *div = (const uint8_t *)find_str((const char *)data, r, "tfhd");
				const uint8_t *bas_mv = (const uint8_t *)find_str((const char *)data, r, "mvhd");
				const uint8_t *bas_md = (const uint8_t *)find_str((const char *)data, r, "mdhd");

				int show_msg = 1;
				if(bas)
				{
					te->time_bas = rd32_be(bas + 12);
					if(te->time_bas < 1 || te->time_bas > 122880) te->time_bas = te->h_fctx->streams[i]->codec->time_base.den;
					//TRACE(TRACE_INFO, "HLS", "Timescale bas = %i", (int)te->time_bas);
				}

				if(div)
				{
					uint32_t flags = rd32_be(div+4);
					int flag_pos = 8;

					if(flags & 0x01) flag_pos+=8;
					if(flags & 0x02) flag_pos+=8;
					if(flags & 0x08)
					{
						te->time_div = rd32_be(div + flag_pos);
						//TRACE(TRACE_INFO, "HLS", "0 Timescale div = %i", (int)te->time_div);
					}

					if(te->time_div && te->time_bas>=12288 && (float)te->time_bas/(float)(te->time_div) == te->time_bas/te->time_div)
					{
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video timescale: %i/%i", (int)te->time_bas, (int)te->time_div);
						te->time_bas/=te->time_div;
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video framerate: %i fps", (int)te->time_bas);
						show_msg = 0;
					}
					else
					{
						//TRACE(TRACE_INFO, "HLS", "0 Timescale div = %i", (int)te->time_div);
						if(te->time_bas==120 && current_fps == 60000 && te->time_div == 1) {te->time_bas/=2;}
						else if(te->time_bas==100 && current_fps == 50000 && te->time_div == 1) {te->time_bas/=2;}
						//TRACE(TRACE_DEBUG, "fMP4-MAP-VA", "Video timescale: %i/%i", (int)te->time_bas, (int)te->time_div);
					}
				}
				else
				{
					//TRACE(TRACE_ERROR, "fMP4-MAP-VB", "Video timescale: %i/%i, fps: %i", (int)te->time_bas, (int)te->time_div, current_fps);

					if(te->time_bas>15360 && (float)te->time_bas/512.f == te->time_bas/512 && current_fps == 60000) {te->time_bas/=512; te->time_div = 512;}
					else if(te->time_bas>15360 && (float)te->time_bas/512.f == te->time_bas/512 && current_fps == 50000) {te->time_bas/=512; te->time_div = 512;}
					else if(te->time_bas==120000 && current_fps == 59940) {te->time_div = 2002;}
					else if(te->time_bas==120 && current_fps == 60000) {te->time_bas = 60; te->time_div = 1;}
					else if(te->time_bas==120 && current_fps == 30000) {te->time_bas = 30; te->time_div = 1;}
					else if(te->time_bas==60000 && current_fps == 59940) {te->time_div = 1001;}
					else if(te->time_bas==60000 && current_fps == 29970) {te->time_div = 2002;}
					else if(te->time_bas==60 && current_fps == 30000) {te->time_bas = 30; te->time_div = 1;}
					else if(te->time_bas==100000 && current_fps == 50000) {te->time_div = 2000;}
					else if(te->time_bas==100 && current_fps == 50000) {te->time_bas = 50; te->time_div = 1;}
					else if(te->time_bas==100 && current_fps == 25000) {te->time_bas = 25; te->time_div = 1;}
					else if(te->time_bas==50000 && current_fps == 50000) {te->time_div = 1000;}
					else if(te->time_bas==50000 && current_fps == 25000) {te->time_div = 2000;}
					else if(te->time_bas==50 && current_fps == 25000) {te->time_bas = 25; te->time_div = 1;}
					else if(te->time_bas==48000 && current_fps == 23976) {te->time_div = 2002;}
					else if(te->time_bas==30000 && current_fps == 29970) {te->time_div = 1001;}
					else if(te->time_bas==24000 && current_fps == 23976) {te->time_div = 1001;}

					else if(te->time_bas>15360 && (float)te->time_bas/1024.f == te->time_bas/1024) {te->time_bas/=1024; te->time_div = 1024;}
					else if((float)te->time_bas/512.f == te->time_bas/512) { te->time_bas/=512; te->time_div = 512; }
					//TRACE(TRACE_ERROR, "fMP4-MAP-V", "Video timescale: %i/%i", (int)te->time_bas, (int)te->time_div);
					else
					if(bas_mv || bas_md)
					{
						if((bas_mv && (rd32_be(bas_mv + 16)==50)) || (bas_md && (rd32_be(bas_md + 16)==50))) { if(current_fps == 25000) te->time_bas = 25; else te->time_bas = 50; te->time_div = 1; show_msg = 0; }
						if((bas_mv && (rd32_be(bas_mv + 16)==60)) || (bas_md && (rd32_be(bas_md + 16)==60))) { if(current_fps == 30000) te->time_bas = 30; else te->time_bas = 60; te->time_div = 1; show_msg = 0; }
						if(!show_msg) TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video timescale: %i/%i (mvhd/mdhd)", (int)te->time_bas, (int)te->time_div);
					}

					//TRACE(TRACE_ERROR, "fMP4-MAP-VC", "Video timescale: %i/%i, fps: %i", (int)te->time_bas, (int)te->time_div, current_fps);
				}

				free(data);

				//TRACE(TRACE_ERROR, "fMP4-MAP-B", "hd->hd_current->hv_fps: %.3f (%i)", hd->hd_current->hv_fps, (int)(hd->hd_current->hv_fps*1000.f));
				if( show_msg )
				{
					if( te->time_div==1001 || te->time_div==2002 || te->time_div==512 || te->time_div==1024 )
					{
						if(te->time_div==512 || te->time_div==1024 )
						{
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video timescale: %i/%i", (int)te->time_bas*te->time_div, te->time_div);
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video framerate: %.3f fps (%.3f)", ((float)te->time_bas*(float)te->time_div) / (float)te->time_div, hd->hd_current->hv_fps);
						}
						else
						{
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video timescale: %i/%i", (int)te->time_bas, te->time_div);
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video framerate: %.3f fps (%.3f)", (float)te->time_bas / (float)te->time_div, hd->hd_current->hv_fps);
						}
					}
					else if( te->time_div==1001 || (te->time_bas<=60 && te->time_div ) )
					{
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video timescale: %i/%i", (int)te->time_bas, te->h_fctx->streams[i]->codec->time_base.num);
						TRACE(TRACE_DEBUG, "fMP4-MAP-V", "Video framerate: %.3f fps", (float)te->time_bas / (float)te->h_fctx->streams[i]->codec->time_base.num);
					}
				}
			}
		}
	}
	else
		{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: video avformat_find_stream_info()");*/ return HLS_EOF;}
}
else
	{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: video avformat_open_input()");*/ return HLS_EOF;}



					if(te->h_ctx && te->h_ctx->codec_id)
					{
						mcp.extradata = te->h_ctx->extradata;	// important for Apple VTB (Linux)
						mcp.extradata_size = te->h_ctx->extradata_size;
						//hexdump("MCPEXT", mcp.extradata, MIN(mcp.extradata_size, 64));
						mcp.profile = te->h_ctx->profile;
						mcp.level = te->h_ctx->level;
						mcp.sar_num = te->h_ctx->sample_aspect_ratio.num;
						mcp.sar_den = te->h_ctx->sample_aspect_ratio.den;

						//TRACE(TRACE_ERROR, "HLS", "mcp.sar_num=%i mcp.sar_den=%i mcp.profile=%i mcp.level=%i", mcp.sar_num, mcp.sar_den, mcp.profile, mcp.level);

						need_rewind = 0;
						td->td_mp->mp_hls_source |= 2; // used in android_audio to use the first audio TS
						td->td_mp->mp_hls_source &= 0xfe;
						if(td->td_hd->hd_hls->h_codec_h264 != NULL && td->td_hd->hd_hls->h_codec_h264->codec_id == te->h_ctx->codec_id)
						{
							te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);

							td->td_hd->hd_hls->h_codec_h264->fmt_ctx->extradata = te->h_ctx->extradata;
							td->td_hd->hd_hls->h_codec_h264->fmt_ctx->extradata_size = te->h_ctx->extradata_size;
#if defined(__ANDROID__)
							td->td_hd->hd_hls->h_codec_h264->avc_csd_epoch++;
#endif
							//TRACE(TRACE_DEBUG, "HLS", "New csd epoch: %i", td->td_hd->hd_hls->h_codec_h264->avc_csd_epoch);
							//mp_flush(td->td_hd->hd_hls->h_codec_h264->mp);
							mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PAUSE));
							usleep(333333);
							need_rewind = 1;
						}
						else
							te->te_codec = media_codec_create(te->h_ctx->codec_id, 0, NULL, te->h_ctx, &mcp, td->td_mp);

						td->td_hd->hd_hls->h_codec_h264 = te->te_codec;
						/*
						td->td_hd->hd_hls->time_bas = te->time_bas;
						td->td_hd->hd_hls->time_num = te->time_num;
						td->td_hd->hd_hls->time_den = te->time_den;
						td->td_hd->hd_hls->time_div = te->time_div;
						td->td_hd->hd_hls->time_ok = te->time_ok;
						*/

						//te->te_codec->fmt_ctx = te->h_ctx;
					}
					else
						{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: video media_codec_create()");*/ return HLS_EOF;}

					if(!te->te_codec)
						{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: video media_codec_create()=NULL");*/ return HLS_EOF;}

					//else
					//te->te_codec = media_codec_create(AV_CODEC_ID_H264, 0, NULL, td->td_hd->hd_hls->h_ctx_video, &mcp, td->td_mp);
					//TRACE(TRACE_DEBUG, "HLS", "Codec created: AVC");


//if(te->h_avio) fa_libav_close(te->h_avio); te->h_avio = NULL;
//fa_libav_close_format(fctx, 0);
				}
				if(!te->h_fh) return HLS_EOF;
				fa_close(te->h_fh);

//set_te_video:
				te->te_data_type = MB_VIDEO;
				td->td_mp->mp_hls_source &= 0xfe;
				td->td_mp->mp_hls_source |= 2; // used in android_audio to use the first audio TS
				if(!hv->hv_frozen) td->td_mp->mp_hls_source |= 4;
				te->te_stream = 0;
				te->te_probe_frame = 0;

				if(td->td_buf_fmp4==NULL)
				{
					td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, TD_BUF_SIZE_V);
					//TRACE(TRACE_DEBUG, "fMP4-DATA", "AUDIO-BUF: %i bytes", TD_BUF_SIZE);
					td->td_buf_size = TD_BUF_SIZE_V;
				}

				/*memcpy(td->td_buf, buf_c8(hv->hv_map), buf_size(hv->hv_map));
				r = fa_read(hs->hs_fh, td->td_buf+buf_size(hv->hv_map), sizeof(td->td_buf)-buf_size(hv->hv_map));
				hd->hd_download_counter += r;
				unmuxed_input(td, td->td_buf, buf_size(hv->hv_map)+r, hs);
				*/

				te->te_bts = te->te_dts = te->te_pts = 0;//PTS_UNSET;
				//unmuxed_input(td, buf_data(hv->hv_map), buf_size(hv->hv_map), hs);

				//if(te->h_avio)
				//	fa_libav_close(te->h_avio);

				//fa_libav_close_format(te->h_fctx, 0);


				/*te->h_avio = NULL;
				te->h_fctx = NULL;
				te->h_ctx = NULL;
				te->h_fh = NULL;
				*/

			}

			if(hd == &h->h_audio/* || td->td_muxed_audio*/)
			{
				ts_es_t *te = find_es(td, 1, 1);

				if(te->te_codec != NULL &&
					(	// ( te->te_codec->codec_id != AV_CODEC_ID_AAC && te->te_codec->codec_id != AV_CODEC_ID_AC3 && te->te_codec->codec_id != AV_CODEC_ID_EAC3 ) ||
						te->te_stream != hv->hv_audio_stream)
					)
				{
					//TRACE(TRACE_ERROR, "fMP4", "Switching audio MAP");
					media_codec_deref(te->te_codec);
					te->te_codec = NULL;
					if(te->h_avio)
						fa_libav_close(te->h_avio);

					te->h_avio = NULL;
					te->h_fctx = NULL;
					te->h_ctx = NULL;
					te->h_fh = NULL;
				}

				if(te->te_codec == NULL)
				{

//if(te->h_fctx != NULL) fa_libav_close_format(te->h_fctx, 0);

te->h_fmt = av_find_input_format("m4a");

//te->h_fh = fa_open(rstr_get(hs->hs_map_url), NULL, 0);
/*
TRACE(TRACE_ERROR, "HLS", "URL: %s", hs->hs_url);
TRACE(TRACE_ERROR, "HLS", "MAP: %s", rstr_get(hs->hs_map_url));
if(!strcmp(hs->hs_url, rstr_get(hs->hs_map_url)))
	TRACE(TRACE_INFO, "HLS", "MAP=URL");
*/

int flags = 0;//(FA_BUFFERED_NO_PREFETCH | FA_BUFFERED_SMALL);
//int flags = (FA_BUFFERED_NO_PREFETCH | FA_BUFFERED_SMALL);

if(strcmp(hs->hs_url, rstr_get(hs->hs_map_url)))
	flags |= FA_NO_PARKING;

for(int t=0;t<10;t++)
{
	te->h_fh = fa_open_ex(rstr_get(hs->hs_map_url), NULL, 0, flags, NULL);
	if(te->h_fh) break;
	usleep(500000);
}

if(!te->h_fh) { TRACE(TRACE_ERROR, "fMP4", "Unable to open audio MAP file"); return HLS_EOF;}
te->h_avio = fa_libav_reopen(te->h_fh, 1);
if(!te->h_avio) { TRACE(TRACE_ERROR, "fMP4", "Audio MAP AVIO failed"); return HLS_EOF;}
//TRACE(TRACE_DEBUG, "fMP4-MAP-A", "%s: Probed as %s", rstr_get(hs->hs_map_url), te->h_fmt->name); //, avio_size(te->h_avio)
//te->h_avio->direct = 1;
te->h_fctx = avformat_alloc_context();
te->h_fctx->pb = te->h_avio;
te->h_fctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
//te->h_fctx->flags = AVFMT_FLAG_FLUSH_PACKETS;

if(!avformat_open_input(&te->h_fctx, rstr_get(hs->hs_map_url), te->h_fmt, NULL))
{
	te->h_fctx->fps_probe_size = 0;
    te->h_fctx->max_analyze_duration = 1000000;//1000000;
	te->h_fctx->probesize = 65536;
	if(!avformat_find_stream_info(te->h_fctx, NULL))
	{
		//fa_seek(te->h_fh, 0, SEEK_SET);
		// bw_start = arch_get_avtime();
		int map_ok = 0;
		uint8_t *data = calloc(1, 128*1024);
		int r = fa_read(te->h_fh, data, 128*1024);

		if(r > 0) add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, r);

		if(r < 100)
		{
			fa_seek(te->h_fh, 0, SEEK_SET);
			r = fa_read(te->h_fh, data, 128*1024);
		}

		for (int i = 0; i < te->h_fctx->nb_streams; i++)
		{
			if(te->h_fctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO) continue;
			//if(te->h_fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
			//	TRACE(TRACE_DEBUG, "probe", "%i VIDEO", i);

			//AVStream *stream = te->h_fctx->streams[i];
			te->h_ctx = te->h_fctx->streams[i]->codec;
			char str[256];

			avcodec_string(str, sizeof(str), te->h_ctx, 0);
			TRACE(TRACE_INFO, "fMP4-MAP-A", "%s", str);
			//TRACE(TRACE_DEBUG, "fMP4-MAP-A", "Audio timescale: %i/%i (SR: %i Hz)", te->h_fctx->streams[i]->codec->time_base.den, te->h_fctx->streams[i]->codec->time_base.num, te->h_fctx->streams[i]->codec->sample_rate);
			//TRACE(TRACE_DEBUG, "fMP4-MAP-A", "Audio channels: %i", te->h_fctx->streams[i]->codec->channels);

			te->time_bas = te->h_fctx->streams[i]->codec->time_base.den;
			te->time_div = te->h_fctx->streams[i]->codec->time_base.num;

			te->time_den = te->h_fctx->streams[i]->codec->time_base.den;
			te->time_num = te->h_fctx->streams[i]->codec->time_base.num;

			const uint8_t *bas = (const uint8_t *)find_str((const char *)data, r, "sidx");
			const uint8_t *div = (const uint8_t *)find_str((const char *)data, r, "tfhd");
			const uint8_t *bas_mv = (const uint8_t *)find_str((const char *)data, r, "mvhd");
			const uint8_t *bas_md = (const uint8_t *)find_str((const char *)data, r, "mdhd");

			if(bas)
			{
				te->time_bas = rd32_be(bas + 12);
				//TRACE(TRACE_INFO, "HLS", "0 Timescale bas = %i", (int)te->time_bas);
				if(te->time_bas < 1 || te->time_bas > 384000) te->time_bas = te->h_fctx->streams[i]->codec->time_base.den;
			}

			if(div)
			{
				uint32_t flags = rd32_be(div+4);
				int flag_pos = 8;

				if(flags & 0x01) flag_pos+=8;
				if(flags & 0x02) flag_pos+=8;
				if(flags & 0x08)
				{
					te->time_div = rd32_be(div + flag_pos);
					//TRACE(TRACE_INFO, "HLS", "0 Timescale div = %i", (int)te->time_div);
				}
				if(te->time_div<1 || te->time_div>16384) te->time_div = te->h_fctx->streams[i]->codec->time_base.num;
			}

			te->time_sr = te->h_fctx->streams[i]->codec->sample_rate;

			if(!bas || (te->time_sr != 44100 && te->time_sr != 48000))
			{
				if(bas_mv && (rd32_be(bas_mv + 16)==48000 || rd32_be(bas_mv + 16)==44100))
				{ te->time_sr = te->time_bas = rd32_be(bas_mv + 16); te->time_div = 1; }
				else
				if(bas_md && (rd32_be(bas_md + 16)==48000 || rd32_be(bas_md + 16)==44100))
				{ te->time_sr = te->time_bas = rd32_be(bas_md + 16); te->time_div = 1; }
				else
				if(bas_mv && (rd32_be(bas_mv + 16)==24000 || rd32_be(bas_mv + 16)==22050))
				{te->time_sr = rd32_be(bas_mv + 16); te->time_bas = rd32_be(bas_mv + 16)*2; te->time_div = 1;}
				else
				if(bas_md && (rd32_be(bas_md + 16)==24000 || rd32_be(bas_md + 16)==22050))
				{te->time_sr = rd32_be(bas_md + 16); te->time_bas = rd32_be(bas_md + 16)*2; te->time_div = 1;}
				else
				if(te->time_sr != 44100 && te->time_sr != 48000)
				{te->time_sr = te->time_bas = 48000; te->time_div = 1;}
				if(te->time_bas < 1 || te->time_bas > 384000) te->time_bas = te->h_fctx->streams[i]->codec->time_base.den;

				TRACE(TRACE_DEBUG, "fMP4-MAP-A", "Audio timescale: %i/%i (SR: %i Hz)%s", (int)te->time_sr, (int)te->time_div, te->time_bas, ((bas_mv||bas_md)?" (mvhd/mdhd)":""));
			}
			else
				TRACE(TRACE_DEBUG, "fMP4-MAP-A", "Audio timescale: %i/%i (SR: %i Hz)", (int)te->time_bas, (int)te->time_div, te->time_sr);

			map_ok = 1;
			//te->te_start = arch_get_avtime();
			//TRACE(TRACE_DEBUG, "fMP4-MAP", "channel_layout=%llu", te->h_fctx->streams[i]->codec->channel_layout);
		}
		if(!map_ok) {te->time_sr = te->time_bas = 48000; te->time_div = 1;}

		if(data) free(data);
	}
	else
		{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: audio avformat_find_stream_info()");*/ return HLS_EOF;}
}
else
	{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: audio avformat_open_input()");*/ return HLS_EOF;}

					if(te->h_ctx && te->h_ctx->codec_id)
					{
						td->td_mp->mp_hls_source |= 2; // used in android_audio to use the first audio TS
						td->td_mp->mp_hls_source &= 0xfe;
						media_codec_params_t mcp = {0};
						mcp.extradata = te->h_ctx->extradata;
						mcp.extradata_size = te->h_ctx->extradata_size;
						te->te_codec = media_codec_create(te->h_ctx->codec_id, 0, NULL, te->h_ctx, &mcp, td->td_mp);
						//te->te_codec->fmt_ctx = te->h_ctx;
					}
					else
						{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: audio media_codec_create()");*/ return HLS_EOF;}

					if(!te->te_codec)
						{ fa_close(te->h_fh); /*TRACE(TRACE_ERROR, "fMP4", "Error: audio media_codec_create()=NULL");*/ return HLS_EOF;}

//if(te->h_avio) fa_libav_close(te->h_avio); te->h_avio = NULL;
//fa_libav_close_format(fctx, 0);
//					TRACE(TRACE_DEBUG, "HLS", "Codec created: AAC");
					if(!te->h_fh) return HLS_EOF;

					td->td_mp->mp_hls_source |= 2; // used in android_audio to use the first audio TS
					td->td_mp->mp_hls_source &= 0xfe;
					if(!hv->hv_frozen) td->td_mp->mp_hls_source |= 4;
					te->te_data_type = MB_AUDIO;
					te->te_stream = hv->hv_audio_stream;  //hls_get_audio_track(hd->hd_hls, 1, hd->hd_current->hv_url, NULL, "AAC", 1);
					te->te_probe_frame = 0;

					if(td->td_buf_fmp4==NULL)
					{
						td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, TD_BUF_SIZE_A);
						//TRACE(TRACE_DEBUG, "fMP4-DATA", "AUDIO-BUF: %i bytes", TD_BUF_SIZE);
						td->td_buf_size = TD_BUF_SIZE_A;
					}

					char acname[8];
					sprintf(acname, "%s", "AAC");
					if(te->te_codec->codec_id == AV_CODEC_ID_EAC3) sprintf(acname, "%s", "EAC3");
					else if(te->te_codec->codec_id == AV_CODEC_ID_AC3) sprintf(acname, "%s", "AC3");
					else if(te->te_codec->codec_id == AV_CODEC_ID_DTS) sprintf(acname, "%s", "DTS");
					else if(te->te_codec->codec_id == AV_CODEC_ID_MP3 || te->te_codec->codec_id == AV_CODEC_ID_MP2) sprintf(acname, "%s", "MP3");

					if(te->h_fctx->nb_streams && te->h_fctx->streams[0]->codec->channels)
					{
						char num_ch[8];
						sprintf(num_ch, "%i", te->h_fctx->streams[0]->codec->channels);
						hls_get_audio_track(hd->hd_hls, te->te_stream, hd->hd_current->hv_url, NULL, acname, num_ch, 1);
					}
					else
						hls_get_audio_track(hd->hd_hls, te->te_stream, hd->hd_current->hv_url, NULL, acname, NULL, 1);

					//te->te_sample_rate = 0;

					/*memcpy(td->td_buf, buf_c8(hv->hv_map), buf_size(hv->hv_map));
					r = fa_read(hs->hs_fh, td->td_buf+buf_size(hv->hv_map), sizeof(td->td_buf)-buf_size(hv->hv_map));
					hd->hd_download_counter += r;
					unmuxed_input(td, td->td_buf, buf_size(hv->hv_map)+r, hs);

					*/
					te->te_bts = te->te_dts = te->te_pts = 0;
					//unmuxed_input(td, buf_data(hv->hv_map), buf_size(hv->hv_map)+r, hs);

					fa_close(te->h_fh);

				}
			}
		}

	}
#endif

    switch(td->td_mux_mode)
	{

#if USE_FMP4
	case TD_MUX_MODE_FMP4:

      //r = fa_read(hs->hs_fh, td->td_buf_fmp4, sizeof(td->td_buf_fmp4));

		if(cancellable_is_cancelled(hd->hd_cancellable))
		{
			hls_segment_close(hs);

			return NULL; //HLS_EOF;
		}

		if(hd == &h->h_audio)
		{

			//TRACE(TRACE_DEBUG, "fMP4-DATA", "AUDIO: %s", hs->hs_url);
			r = probe_fmp4(td, /*td->td_buf_fmp4,*/ td->td_buf_size, hs, MB_AUDIO, hd);

			hls_segment_close(hs);

			if(r == -1)
				return HLS_EOF;

			if(r == 0)
				continue;

			if(r == -2)
			{
				continue;
				//hls_bad_variant(hv, HLS_ERROR_VARIANT_UNKNOWN_AUDIO);
				//return NULL;
			}

		}

		if(hd == &h->h_primary)
		{
			if(need_rewind)
			{
				need_rewind = 0;
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(td->td_hd->hd_hls->h_codec_h264->mp, event_create_action(ACTION_PLAY));
				hls_segment_close(hs); return NULL;
			}
			r = probe_fmp4(td, /*td->td_buf_fmp4,*/ td->td_buf_size, hs, MB_VIDEO, hd);

			hls_segment_close(hs);

			if(r == -1)
				return HLS_EOF;

			if(r == 0)
				continue;

			if(r == -2)
			{
				continue;
				//hls_bad_variant(hv, HLS_ERROR_VARIANT_UNKNOWN_AUDIO);
				//return NULL;
			}
		}

		break;
#endif

    case TD_MUX_MODE_UNSET:
		HLS_TRACE(h, "Probing variant %s, sequence %d",
                hv->hv_name, hs->hs_seq);

		if(td->td_buf_fmp4==NULL)
		{
			td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, TD_BUF_SIZE_V);
			//TRACE(TRACE_INFO, "fMP4-DATA", "TD_MUX_MODE_UNSET-BUF: %i bytes", TD_BUF_SIZE_V);
			td->td_buf_size = TD_BUF_SIZE_V;
		}

		if(!hv->hv_frozen) td->td_mp->mp_hls_source |= 4;
#if defined(__ANDROID__)
		td->td_mp->mp_start_time = (hv->hv_start_time_offset==PTS_UNSET?0:hv->hv_start_time_offset);
#endif
		xr=0;
		// bw_start = arch_get_avtime();

		while(1)
		{
			td->td_last_r = r = fa_read(hs->hs_fh, td->td_buf_fmp4+xr, td->td_buf_size-xr);
			if(r < 0) break;

			xr+=r;
			//TRACE(TRACE_INFO, "TS-DATA", "TD_MUX_MODE_UNSET-BUF: %i read, total=%i", r, xr);

			if(xr > td->td_buf_size-16356)
			{
				td->td_buf_size = xr + 4194280; //(xr+(4*1024*1024)-1) & -(1024*1024);
				td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, td->td_buf_size);
				//TRACE(TRACE_INFO, "TS-DATA", "td_buf_fmp4 REALLOC +4MB: %i bytes", td->td_buf_size);
			}

			if(r == 0)
			{
				td->td_last_r = r = xr;
				break;
			}
		}

		if(xr > 0) add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, xr);

        if(cancellable_is_cancelled(hd->hd_cancellable))
		{
			td->td_buf_bytes = 0;
			return NULL;//HLS_EOF;
		}

		if(r == 0)
		{
			td->td_buf_bytes = 0;
			hls_segment_close(hs);
			continue;
		}

        if(r < 0)
		{
			td->td_buf_bytes = 0;
			TRACE(TRACE_ERROR, "HLS", "Segment probe error");
			hls_bad_variant(hv, HLS_ERROR_VARIANT_PROBE_ERROR);
			return HLS_DIS;//NULL;
        }

		  // Search stream for TS mux lock, we want two continous packets

		int i;
		if(memcmp(td->td_buf_fmp4, "ID3", 3))
		{
			if(r >= 2*188)
			{
				for(i = 0; i < r - 188 * 2; i++)
				{
					if(td->td_buf_fmp4[i] == 0x47
					&& td->td_buf_fmp4[i + 188] == 0x47
					//&& td->td_buf_fmp4[i + 188 * 2] == 0x47
					)
					{
						//TRACE(TRACE_INFO, "fMP4-DATA", "3 - FOUND TS LOCK at %i", i);
						//if(td->td_buf_fmp4[i + 188 * 2] != 0x47)
						//	TRACE(TRACE_ERROR, "HLS", "TS_LOCK at %i (but no third packet)", i);
						break;
					}
				}

				if(i != r - 188 * 2)
				{
					td->td_mux_mode = TD_MUX_MODE_TS;
					if(hs->hs_crypto!=HLS_CRYPTO_NONE)
						HLS_TRACE(h, "Variant %s is ENCRYPTED transport stream", hv->hv_name);
					else
						HLS_TRACE(h, "Variant %s is a transport stream", hv->hv_name);

					//while(i <= 1*188 - 188)
					{
						process_tsb(td, td->td_buf_fmp4 + i, hs);
						i += 188;
					}
					td->td_buf_bytes = i;

					break;
				}
			}
		}

		if(hd == &h->h_primary)
		{
			if(attempts < 10)
			{
				attempts++;
				hls_segment_close(hs);
				continue;
			}

			hls_bad_variant(hv, HLS_ERROR_VARIANT_NO_VIDEO);
			return NULL;
		}

		if(hd == &h->h_audio && probe_non_muxed_audio(td, td->td_buf_fmp4, r, hs))
		{
			if(attempts < 10)
			{
				attempts++;
				hls_segment_close(hs);
				continue;
			}

			hls_bad_variant(hv, HLS_ERROR_VARIANT_UNKNOWN_AUDIO);
			return NULL;
		}

		if(hd == &h->h_subtitle)
		{
			if(probe_non_muxed_subtitle(td, td->td_buf_fmp4, r, hs))
			{
				hls_bad_variant(hv, HLS_ERROR_VARIANT_UNKNOWN_SUBTITLE);
				return NULL;
			}

			hls_segment_close(hs);
			td->td_mux_mode = TD_MUX_MODE_RAW;
			//td->td_buf_bytes = 0;
			memset(td->td_buf_fmp4, 0, td->td_buf_size);

			return NULL;
		}

		td->td_mux_mode = TD_MUX_MODE_RAW;
		//td->td_buf_bytes = 0;
		hls_segment_close(hs);
		continue;

    case TD_MUX_MODE_RAW:

		if(td->td_buf_fmp4==NULL)
		{
			td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, TD_BUF_SIZE_A);
			//TRACE(TRACE_DEBUG, "fMP4-DATA", "TD_MUX_MODE_RAW-BUF: %i bytes", TD_BUF_SIZE_V);
			td->td_buf_size = TD_BUF_SIZE_A;
		}

		xr=0;
		// bw_start = arch_get_avtime();
		while(1)
		{
			r = fa_read(hs->hs_fh, td->td_buf_fmp4+xr, td->td_buf_size-xr);
			if(r < 0) break;
			xr+=r;

			if(xr >= td->td_buf_size-16356)
			{
				td->td_buf_size = xr + 4194280; //(xr+(4*1024*1024)-1) & -(1024*1024);
				td->td_buf_fmp4 = myreallocf(td->td_buf_fmp4, td->td_buf_size);
				//TRACE(TRACE_INFO, "fMP4-DATA", "td_buf_fmp4 REALLOC +4MB: %i bytes", td->td_buf_size);
			}

			if(r == 0)
			{
				r = xr;
				break;
			}
		}

		if(xr > 0) add_speed(hd->hd_hls->h_mp->mp_video.mq_prop_bw, xr);
		//TRACE(TRACE_INFO, "fMP4-DATA", "TD_MUX_MODE_RAW-BUF: %i read, total=%i", r, xr);

		if(cancellable_is_cancelled(hd->hd_cancellable))
			return NULL;//HLS_EOF;

		if(r < 0)
			return HLS_EOF;

		if(r == 0)
		{
			hls_segment_close(hs);
			continue;
		}
		//hd->hd_download_counter += r;
		//TRACE(TRACE_DEBUG, "fMP4-DATA", "TD_MUX_MODE_RAW-BUF: %i read, total=%i (unmuxed_input)", r, xr);
		unmuxed_input(td, td->td_buf_fmp4, r, hs);
		break;

    case TD_MUX_MODE_TS:

		if(cancellable_is_cancelled(hd->hd_cancellable))
			return NULL;//HLS_EOF;

		for(int tsp=0;tsp<3;tsp++)
		{
/*
      r = fa_read(hs->hs_fh,
                  td->td_buf + td->td_buf_bytes,
                  188 - td->td_buf_bytes);
*/
			if(td->td_buf_bytes >= td->td_last_r-187)
			{
				r = 0;
				td->td_buf_bytes = 0;
			}
			else
				r = td->td_last_r - td->td_buf_bytes;

			if(r < 0)
				return HLS_EOF;

			if(r == 0)
			{
				break;
				//hls_segment_close(hs);
				//continue;
			}

			process_tsb(td, td->td_buf_fmp4 + td->td_buf_bytes, hs);
			td->td_buf_bytes += 188;
		}
		//TRACE(TRACE_DEBUG, "TS-DATA", "TD_MUX_MODE_TS-BUF: [%i] r=%i, td->td_buf_bytes=%i", 0, r, td->td_buf_bytes);

		if(r == 0)
		{
			hls_segment_close(hs);
			continue;
		}

      break;
    }
  }

  return NULL;
}
