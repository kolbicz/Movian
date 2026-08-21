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

#include "navigator.h"
#include "backend/backend.h"
#include "media/media.h"
#include "main.h"
#include "i18n.h"
#include "misc/isolang.h"
#include "misc/str.h"
#include "misc/dbl.h"
#include "misc/queue.h"
#include "video/video_playback.h"
#include "video/video_settings.h"
#include "metadata/playinfo.h"
#include "fileaccess/fileaccess.h"
#include "fileaccess/fa_libav.h"

#include <libavformat/avformat.h>

#include "hls.h"
#include "subtitles/subtitles.h"
#include "usage.h"
#include "misc/minmax.h"

#if LAST_ACTION
extern volatile int64_t f_last_action;
#endif

static int last_bw = -1;
static int64_t last_pos = -1;

#define HLS_CORRUPTION_MEASURE_PERIOD (60 * 1000000)

/**
 * Relevant docs:
 *
 * http://tools.ietf.org/html/draft-pantos-http-live-streaming-07
 * http://developer.apple.com/library/ios/#technotes/tn2288/
 * http://developer.apple.com/library/ios/#technotes/tn2224/
 *
 * Buffer-Based Rate Adaptation for HTTP Video Streaming
 *    http://conferences.sigcomm.org/sigcomm/2013/papers/fhmn/p9.pdf
 */

#define TESTURL "http://devimages.apple.com.edgekey.net/resources/http-streaming/examples/bipbop_16x9/bipbop_16x9_variant.m3u8"


static void
hls_seek(hls_t *h, int64_t ts);

static hls_variant_t *hls_demuxer_select_variant(hls_demuxer_t *hd, int64_t now,
                                                 int bw);


/**
 *
 */
static void
hls_free_mbp(media_pipe_t *mp, media_buf_t **mbp)
{
  media_buf_t *mb = *mbp;
  if(*mbp == NULL)
    return;

  if(mb != HLS_EOF && mb != HLS_DIS)
    media_buf_free_unlocked(mp, mb);
  *mbp = NULL;
}


/**
 *
 */
static char *
get_attrib(char *v, const char **keyp, const char **valuep)
{
  const char *key = v;
  while(*key == ' ')
    key++;

  char *value = strchr(key, '=');
  if(value == NULL)
    return NULL;
  *value++ = 0;
  while(*value < 33 && *value)
    value++;
  if(*value == '"') {
    v = ++value;
    while(*v && *v != '"' && v[-1] != '\\')
      v++;
    if(*v)
      *v++ = 0;
  } else {
    v = value;
  }
  while(*v && *v != ',')
    v++;
  if(*v)
     *v++ = 0;
  *keyp = key;
  *valuep = value;
  return v;
}



/**
 *
 */
static hls_discontinuity_segment_t *
discontinuity_seq_get(hls_t *h, int discontinuity_seq)
{
  hls_discontinuity_segment_t *hds;
  LIST_FOREACH(hds, &h->h_discontinuity_segments, hds_link) {
    if(hds->hds_seq == discontinuity_seq) {
      hds->hds_refcount++;
      return hds;
    }
  }

  hds = calloc(1, sizeof(hls_discontinuity_segment_t));
  LIST_INSERT_HEAD(&h->h_discontinuity_segments, hds, hds_link);
  if(discontinuity_seq != 0)
    hds->hds_offset = PTS_UNSET;
  hds->hds_refcount = 1;
  hds->hds_seq = discontinuity_seq;
  return hds;
}


/**
 *
 */
static void
discontinuity_seq_release(hls_discontinuity_segment_t *hds)
{
  if(hds == NULL) return;
  if(--hds->hds_refcount)
    return;
  LIST_REMOVE(hds, hds_link);
  free(hds);
}


/**
 *
 */
static void
segment_destroy(hls_segment_t *hs)
{
  discontinuity_seq_release(hs->hs_discontinuity_segment);

  if(hs->hs_fh != NULL)
    fa_close(hs->hs_fh);

  TAILQ_REMOVE(&hs->hs_variant->hv_segments, hs, hs_link);
  free(hs->hs_url);
  rstr_release(hs->hs_key_url);
  rstr_release(hs->hs_map_url);
  if(hs == hs->hs_variant->hv_segment_search)
    hs->hs_variant->hv_segment_search = NULL;
  free(hs);
}


/**
 *
 */
static void
variant_destroy(hls_variant_t *hv)
{
  hls_segment_t *hs;
  hls_variant_close(hv);

  while((hs = TAILQ_FIRST(&hv->hv_segments)) != NULL)
    segment_destroy(hs);

  free(hv->hv_subs_group);
  free(hv->hv_audio_group);
  buf_release(hv->hv_key);
  rstr_release(hv->hv_key_url);
  //buf_release(hv->hv_map);
  //rstr_release(hv->hv_map_url);
  free(hv->hv_url);
  free(hv);
}


/**
 *
 */
static void
variants_destroy(struct hls_variant_queue *q)
{
  hls_variant_t *hv;
  while((hv = TAILQ_FIRST(q)) != NULL) {
    TAILQ_REMOVE(q, hv, hv_link);
    variant_destroy(hv);
  }
}



/**
 *
 */
static hls_variant_t *
variant_create(hls_demuxer_t *hd)
{
  hls_variant_t *hv = calloc(1, sizeof(hls_variant_t));
  hv->hv_start_time_offset = PTS_UNSET;
  hv->hv_demuxer = hd;
  hv->hv_first_seq = -1;
  hv->hv_last_seq = -1;
  TAILQ_INIT(&hv->hv_segments);
  return hv;
}


/**
 *
 */
static hls_segment_t *
hv_add_segment(hls_variant_t *hv, const char *url)
{
  hls_segment_t *hs = calloc(1, sizeof(hls_segment_t));
  hs->hs_ts_offset = PTS_UNSET;
  hs->hs_url = url_resolve_relative_from_base(hv->hv_url, url);
  hs->hs_variant = hv;
  TAILQ_INSERT_TAIL(&hv->hv_segments, hs, hs_link);
  return hs;
}


/**
 *
 */
typedef struct hls_variant_parser {
  rstr_t *hvp_key_url;
  rstr_t *hvp_map_url;
  int hvp_crypto;
  int hvp_map;
  int hvp_explicit_iv;
  uint8_t hvp_iv[16];
} hls_variant_parser_t;


/**
 *
 */
static void
hv_parse_map(hls_variant_parser_t *hvp, const char *baseurl, const char *V)
{
  char *v = mystrdupa(V);

  while(*v) {
    const char *key, *value;
    v = get_attrib(v, &key, &value);
    if(v == NULL)
      break;

    if(!strcmp(key, "URI")) {
      char *s = url_resolve_relative_from_base(baseurl, value);
      rstr_release(hvp->hvp_map_url);
      hvp->hvp_map_url = rstr_alloc(s);
      free(s);
	  hvp->hvp_map = HLS_MAP_FMP4;
	  return;
    }
  }
  hvp->hvp_map = HLS_MAP_NONE;
}

/**
 *
 */
static void
hv_parse_key(hls_variant_parser_t *hvp, const char *baseurl, const char *V)
{
  char *v = mystrdupa(V);

  hvp->hvp_crypto = HLS_CRYPTO_NONE;
  hvp->hvp_explicit_iv = 0;

  while(*v) {
    const char *key, *value;
    v = get_attrib(v, &key, &value);
    if(v == NULL)
      break;

    if(!strcmp(key, "METHOD")) {
      if(!strcmp(value, "AES-128"))
	hvp->hvp_crypto = HLS_CRYPTO_AES128;

    } else if(!strcmp(key, "URI")) {
      char *s = url_resolve_relative_from_base(baseurl, value);
      rstr_release(hvp->hvp_key_url);
      hvp->hvp_key_url = rstr_alloc(s);
      free(s);

    } else if(!strcmp(key, "IV")) {
      if(!strncmp(value, "0x", 2) || !strncmp(value, "0X", 2)) {
	hvp->hvp_explicit_iv = 1;
	hex2bin(hvp->hvp_iv, sizeof(hvp->hvp_iv), value + 2);
      }
    }
  }
}

/**
 *
 */
static void
hv_parse_start(hls_variant_t *hv, const char *V)
{
  char *v = mystrdupa(V);
  while(*v) {
    const char *key, *value;
    v = get_attrib(v, &key, &value);
    if(v == NULL)
      break;

    if(!strcmp(key, "TIME-OFFSET")) {
      //hv->hv_start_time_offset = MAX(my_str2double(value, NULL), 0.0f) * 1000000;
	  hv->hv_start_time_offset = my_str2double(value, NULL) * 1000000; // allow negative offset
    }
	//TRACE(TRACE_ERROR, "HLS", "hv->hv_start_time_offset=%lld", hv->hv_start_time_offset);
  }
}


/**
 *
 */
void
hls_bad_variant(hls_variant_t *hv, hls_error_t err)
{
  hls_demuxer_t *hd = hv->hv_demuxer;
  hls_t *h = hd->hd_hls;
  int64_t now = arch_get_ts();

  h->h_last_error = err;

  HLS_TRACE(h, "Unable to demux variant %s -- %s",
            hv->hv_name, hlserrstr[err]);
  hv->hv_corrupt_counter++;
  if(hv->hv_corrupt_timer < now - HLS_CORRUPTION_MEASURE_PERIOD) {
    hv->hv_corruptions_last_period = 1;
    hv->hv_corrupt_timer = now;
  } else {
    hv->hv_corruptions_last_period++;
  }

  hls_variant_close(hv);
  usleep(100000);
  hd->hd_req = hls_demuxer_select_variant(hd, now, 0);
  hd->hd_last_switch = now;
}


/**
 *
 */
static hls_error_t attribute_unused_result
hls_variant_update(hls_variant_t *hv, media_pipe_t *mp)
{
  hls_segment_t *hs;
  char errbuf[1024];
  //int changed = 0;

  if(hv->hv_frozen)
    return 0;

  hls_t *h = hv->hv_demuxer->hd_hls;

  hv->hv_loaded = time(NULL);

  buf_t *b = fa_load(hv->hv_url,
                     FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)),
                     FA_LOAD_FLAGS(FA_COMPRESSION),
                     FA_LOAD_CANCELLABLE(h->h_mp->mp_cancellable),
                     NULL);

  if(b == NULL) {
    if(!cancellable_is_cancelled(h->h_mp->mp_cancellable))
      TRACE(TRACE_ERROR, "HLS", "Unable to open variant %s -- %s",
            hv->hv_url, errbuf);
    return HLS_ERROR_VARIANT_NOT_FOUND;
  }



  b = buf_make_writable(b);

  double duration = 0;
  int64_t byte_offset = -1;
  int64_t byte_size = -1;
  int seq = 1;
  int items = 0;
  hls_variant_parser_t hvp;
  int first_seq = -1;
  int discontinuity_seq = -1;
  int fmp4_map_found = 0;

  memset(&hvp, 0, sizeof(hvp));

  LINEPARSE(s, buf_str(b)) {
    const char *v;
	//TRACE(TRACE_DEBUG, "HLS", "%s", s);
    if((v = mystrbegins(s, "#EXTINF:")) != NULL) {
      duration = my_str2double(v, NULL);
    } else if((v = mystrbegins(s, "#EXT-X-ERROR")) != NULL) {
		TRACE(TRACE_ERROR, "HLS", "Variant Error: %s", s);
    } else if((v = mystrbegins(s, "#EXT-X-ENDLIST")) != NULL) {
      hv->hv_frozen = 1;
    } else if((v = mystrbegins(s, "#EXT-X-PLAYLIST-TYPE:VOD")) != NULL) {
      hv->hv_frozen = 1;
    } else if((v = mystrbegins(s, "#EXT-X-TARGETDURATION")) != NULL) {
      hv->hv_target_duration = atoi(v);
    } else if((v = mystrbegins(s, "#EXT-X-KEY:")) != NULL) {
      hv_parse_key(&hvp, hv->hv_url, v);
    } else if((v = mystrbegins(s, "#EXT-X-MAP:")) != NULL) {
      hv_parse_map(&hvp, hv->hv_url, v);
	  fmp4_map_found = 1;
	  //if(hvp.hvp_map_url!=NULL)	hv->hv_map_url = rstr_alloc(rstr_get(hvp.hvp_map_url));
    } else if((v = mystrbegins(s, "#EXT-X-MEDIA-SEQUENCE:")) != NULL) {
      seq = atoi(v);
      if(first_seq == -1)
        first_seq = seq;
    } else if((v = mystrbegins(s, "#EXT-X-DISCONTINUITY-SEQUENCE:")) != NULL) {
      discontinuity_seq = atoi(v);
    } else if(!strcmp(s, "#EXT-X-DISCONTINUITY")) {
      if(discontinuity_seq == -1) {
        hs = hv_find_segment_by_seq(hv, seq - 1);
        if(hs != NULL)
          discontinuity_seq = hs->hs_discontinuity_segment->hds_seq;
        else
          discontinuity_seq = 0;
      }
      discontinuity_seq++;
    } else if((v = mystrbegins(s, "#EXT-X-START:")) != NULL) {
      hv_parse_start(hv, v);
    } else if((v = mystrbegins(s, "#EXT-X-BYTERANGE:")) != NULL) {
      const char *o = strchr(v, '@');
      if(o != NULL)
	  {
        byte_offset = atoi(o+1);
		byte_size = atoi(v);
	  }
	  else if(byte_offset>0 && byte_size>0)
	  {
		//TRACE(TRACE_DEBUG, "HLS", "0 byte_offset=%i byte_size=%i", byte_offset, byte_size);
		byte_offset += byte_size; // previous byte_size .. BYTERANGE:SIZE format
		byte_size = atoi(v);
		//TRACE(TRACE_DEBUG, "HLS", "1 byte_offset=%i byte_size=%i", byte_offset, byte_size);
	  }
	  else
	  {
		  byte_offset = -1;
		  byte_size = -1;
	  }


    } else if(s[0] != '#') {

      items++;


	if(seq > hv->hv_last_seq) {

	if(discontinuity_seq == -1) {
	  hs = hv_find_segment_by_seq(hv, seq - 1);
	  if(hs != NULL && hs->hs_discontinuity_segment != NULL)
		discontinuity_seq = hs->hs_discontinuity_segment->hds_seq;
	  else
		discontinuity_seq = 0;
	}

	if(hv->hv_first_seq == -1)
	{
		hv->hv_first_seq = seq;

		if(!hvp.hvp_key_url && !hvp.hvp_explicit_iv)
		if(!hv->hv_width || !hv->hv_height || !hv->hv_fps)
		{
			const char *s2;
			if(hvp.hvp_map == HLS_MAP_FMP4 && hvp.hvp_map_url)
				s2 = rstr_get(hvp.hvp_map_url);
			else
				s2 = url_resolve_relative_from_base(hv->hv_url, s);
			//TRACE(TRACE_DEBUG, "HLS", "first segment: %s", s2);

			if(s2 != NULL)
			{
				fa_handle_t *h_fh = fa_open_ex(s2, NULL, 0, 0 /*FA_NO_PARKING*/, NULL);
				if(h_fh)
				{
					AVIOContext *avio = fa_libav_reopen(h_fh, 1);
					if(avio)
					{
						AVFormatContext *fctx = avformat_alloc_context();
						AVInputFormat *fmt = NULL;
						if(hvp.hvp_map != HLS_MAP_FMP4 && ((!strstr(s2, ".ts") && !strstr(s2, ".mp4") && !strstr(s2, ".m4v") && !strstr(s2, ".fmp4")) || strstr(s2, ".ts")))
							av_find_input_format("ts");
						else
							av_find_input_format("mp4");
						fctx->pb = avio;
						fctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
						//TRACE(TRACE_ERROR, "HLS", "IN--- %i x %i @ %.3f", hv->hv_width, hv->hv_height, hv->hv_fps);
						if(!avformat_open_input(&fctx, s2, fmt, NULL))
						{
							fctx->fps_probe_size = 120;
							fctx->max_analyze_duration = 2000000;
							fctx->probesize = 65536*2;
							if(!avformat_find_stream_info(fctx, NULL))
							{
								for (int i = 0; i < fctx->nb_streams; i++)
								{
									if(fctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO) continue;

									AVCodecContext *h_ctx = fctx->streams[i]->codec;
									char str[256];
									avcodec_string(str, sizeof(str), h_ctx, 0);

									TRACE(TRACE_DEBUG, (hvp.hvp_map == HLS_MAP_FMP4?"fMP4-MAP-V":"Probe"), "HLS %s %s", (hvp.hvp_map == HLS_MAP_FMP4?"fMP4 Map":"Segment:"), str);
									hv->hv_field_order = h_ctx->field_order;
									//TRACE(TRACE_ERROR, "HLS", "field_order = %i", h_ctx->field_order);
									//TRACE(TRACE_ERROR, "HLS", "field_order = %s", (h_ctx->field_order > AV_FIELD_PROGRESSIVE)?"Interlaced":"Progressive");

									if(fctx->streams[i]->codec->width && fctx->streams[i]->codec->height)
									{
										hv->hv_width = fctx->streams[i]->codec->width;
										hv->hv_height = fctx->streams[i]->codec->height;
										hv->hv_codec = fctx->streams[i]->codec->codec_id;

										if(fctx->streams[i]->codec->framerate.num && fctx->streams[i]->codec->framerate.den)
										{
											hv->hv_fps = ((float)fctx->streams[i]->codec->framerate.num / (float)fctx->streams[i]->codec->framerate.den);
											if(hv->hv_fps<=23.f || hv->hv_fps>60.f) hv->hv_fps = 0.f;
										}

										if(h_ctx->sample_aspect_ratio.num && h_ctx->sample_aspect_ratio.den)
										{
											//TRACE(TRACE_ERROR, "HLS", "sar: %i:%i", h_ctx->sample_aspect_ratio.num, h_ctx->sample_aspect_ratio.den);
											hv->hv_sar_num = h_ctx->sample_aspect_ratio.num;
											hv->hv_sar_den = h_ctx->sample_aspect_ratio.den;
										}

										if(h->h_mp != NULL)
										{
											char info[64];
											if(hv->hv_bitrate) {
											  snprintf(info, sizeof(info), "HLS%s %s%dx%d (%d kb/s)", ((mp->mp_hls_source & 3)==2?" fMP4":""), (hv->hv_codec_hdr?"HDR ":""), hv->hv_width, hv->hv_height, hv->hv_bitrate / 1000);
											} else {
											  snprintf(info, sizeof(info), "HLS%s %s%dx%d", ((mp->mp_hls_source & 3)==2?" fMP4":""), (hv->hv_codec_hdr?"HDR ":""), hv->hv_width, hv->hv_height);
											}
											#if defined(__ANDROID__)
											h->h_mp->mp_hls_source_width = hv->hv_width;
											h->h_mp->mp_hls_source_height = hv->hv_height;
											#endif
											//TRACE(TRACE_ERROR, "HLS", "HLS Info: %s", info);
											prop_set(h->h_mp->mp_prop_metadata, "format", PROP_SET_STRING, info);
										}

										break;
									}
								}
							}
						}
						av_free(fmt);
						av_free(fctx);
						av_free(avio->buffer);
						av_free(avio);
					}
					fa_close(h_fh);
				}
			}
			//TRACE(TRACE_ERROR, "HLS", "OUT-- %i x %i @ %.3f [%i:%i]", hv->hv_width, hv->hv_height, hv->hv_fps, hv->hv_sar_num, hv->hv_sar_den);
		}
    }

	hs = hv_add_segment(hv, s);
	hs->hs_byte_offset = byte_offset;
	hs->hs_byte_size   = byte_size;
	hs->hs_time_offset = hv->hv_duration;
	hs->hs_duration    = duration * 1000000LL;
	hs->hs_crypto      = hvp.hvp_crypto;
	hs->hs_key_url     = rstr_dup(hvp.hvp_key_url);
	hs->hs_map         = hvp.hvp_map;
	hs->hs_map_url     = rstr_dup(hvp.hvp_map_url);
    hs->hs_discontinuity_segment =
          discontinuity_seq_get(h, discontinuity_seq);

	hv->hv_duration   += hs->hs_duration;

	if(hvp.hvp_explicit_iv) {
	  memcpy(hs->hs_iv, hvp.hvp_iv, 16);
	} else {
	  memset(hs->hs_iv, 0, 12);
	  hs->hs_iv[12] = seq >> 24;
	  hs->hs_iv[13] = seq >> 16;
	  hs->hs_iv[14] = seq >> 8;
	  hs->hs_iv[15] = seq;
	}

	if(hv->hv_target_duration == 0)
	  hv->hv_target_duration = duration;

        //changed = 1;

	hs->hs_seq = seq;
	duration = 0;
	//byte_offset = -1;
	//byte_size = -1;
	hv->hv_last_seq = hs->hs_seq;

      }
      seq++;
    }
  }

  if(mp != NULL)
  {
	  //TRACE(TRACE_DEBUG, "HLS", "IN  fmp4_map_found=%i mp->mp_hls_source=%i",fmp4_map_found, mp->mp_hls_source);
	  if(!fmp4_map_found)
		  mp->mp_hls_source |= 1;
	  else
		  mp->mp_hls_source = (mp->mp_hls_source&0xfc)|2;
	  //TRACE(TRACE_INFO, "HLS", "OUT fmp4_map_found=%i mp->mp_hls_source=%i",fmp4_map_found, mp->mp_hls_source);
  }

  if(first_seq == -1)
    first_seq = 1;

  while((hs = TAILQ_FIRST(&hv->hv_segments)) != NULL) {

    if(hs->hs_seq >= first_seq ||
       hs->hs_fh != NULL ||
       hv->hv_current_seg == hs)
      break;
    segment_destroy(hs);
    //changed = 1;
  }

  buf_release(b);
  rstr_release(hvp.hvp_key_url);

  if(TAILQ_FIRST(&hv->hv_segments) == NULL)
  {
	  HLS_TRACE(h, "Variant empty (no segments found)");
      return HLS_ERROR_VARIANT_EMPTY;
  }

#if 0
  if(changed) {
    int first = TAILQ_FIRST(&hv->hv_segments)->hs_seq;
    int last = TAILQ_LAST(&hv->hv_segments, hls_segment_queue)->hs_seq;
    HLS_TRACE(h, "Loaded segments %d ... %d (%d)", first, last, last - first + 1);
  }
#endif
  //if(!hv->hv_frozen && hv->hv_duration)// && video_settings.hls_live_mode>=2)
    h->h_duration = hv->hv_duration;

  return 0;
}

/**
 *
 */
static void
hls_dump_demuxer(const hls_demuxer_t *hd, const hls_t *h)
{
  const hls_variant_t *hv;
  const char *txt;
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    //HLS_TRACE(h, "  %s", hv->hv_url);
    //HLS_TRACE(h, "    bitrate:    %d", hv->hv_bitrate);

    /*if(hv->hv_audio_only) {
      HLS_TRACE(h, "    Audio only\n");
      continue;
    }
    if(hv->hv_initial) {
      HLS_TRACE(h, "    Initial\n");
      continue;
    }
	*/

    switch(hv->hv_h264_profile) {
    case 66:
      txt = "AVC Base";
      break;
    case 77:
      txt = "AVC Main";
      break;
    case 100:
      txt = "AVC High";
      break;
    case 177:
      txt = "HEVC Main 8-bit SDR";
      break;
    case 178:
      txt = "HEVC Main 10-bit HDR";
      break;
    default:
      txt = "<unknown>";
      break;
    }

    HLS_TRACE(h, "    Video: %dx%d, Profile: %s (%d.%d)",
              hv->hv_width, hv->hv_height, txt,
              hv->hv_h264_level / 10,
              hv->hv_h264_level % 10);
  }
}


/**
 *
 */
static void
hls_dump(const hls_t *h)
{
  //HLS_TRACE(h, "Base URL: %s", h->h_baseurl);
  HLS_TRACE(h, "Primary/Adaptive variants");
  hls_dump_demuxer(&h->h_primary, h);
  //HLS_TRACE(h, "Audio variants");
  //hls_dump_demuxer(&h->h_audio, h);
}


/**
 *
 */
static void
check_audio_only(hls_demuxer_t *hd)
{
  hls_variant_t *hv;
  int streams = 0;
  int audio_only = 0;
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    streams++;
    if(hv->hv_audio_only)
      audio_only++;
  }

  if(streams == audio_only) {
    // Most likely not _all_ variants are audio only, so we clear the flag
    TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
      hv->hv_audio_only = 0;
    }
  }
}



/**
 *
 */
hls_error_t
hls_segment_open(hls_segment_t *hs)
{
  hls_variant_t *hv = hs->hs_variant;
  fa_open_extra_t foe = {0};
  fa_handle_t *fh;
  char errbuf[512];
  hls_demuxer_t *hd = hv->hv_demuxer;
  const hls_t *h = hd->hd_hls;

  assert(hs->hs_fh == NULL);
  hs->hs_open_time = arch_get_ts();
  hs->hs_blocked_counter = h->h_blocked;

  foe.foe_open_timeout = 12000;
  foe.foe_cancellable = hd->hd_cancellable;

  int flags = FA_STREAMING;
  //int flags = FA_BUFFERED_BIG | FA_STREAMING;
  //int flags = FA_STREAMING | FA_NO_RETRIES | FA_BUFFERED_NO_PREFETCH | FA_BUFFERED_SMALL | FA_NO_PARKING; // !!!

  if(hs->hs_byte_offset != -1)
    flags &= ~FA_STREAMING;

	fh = fa_open_ex(hs->hs_url, errbuf, sizeof(errbuf), flags, &foe);

	if(fh == NULL)
	{
		if(cancellable_is_cancelled(hd->hd_cancellable))
			return HLS_ERROR_SEGMENT_NOT_FOUND;

		//usleep(500000);
		if(foe.foe_protocol_error == 404 || foe.foe_protocol_error >= 500)
		{
			if(foe.foe_protocol_error >= 500)
			{
				TRACE(TRACE_ERROR, "HLS", "Segment access error: %i", foe.foe_protocol_error);
			}
			return HLS_ERROR_SEGMENT_NOT_FOUND;
		}
		else if(foe.foe_protocol_error == 401 || foe.foe_protocol_error == 403)
		{
			hs->hs_permanent_error = 1;
			return HLS_ERROR_SEGMENT_ACCESS_DENIED;
		}
		else
		{
			return HLS_ERROR_SEGMENT_BROKEN;
		}
	}

  fa_set_read_timeout(fh, 15000);

  if(hs->hs_byte_size != -1 && hs->hs_byte_offset != -1)
    fh = fa_slice_open(fh, hs->hs_byte_offset, hs->hs_byte_size);

  //if(!video_settings.hls_best_quality) hs->hs_size = fa_fsize(fh);

  switch(hs->hs_crypto) {
  case HLS_CRYPTO_AES128:

    if(!rstr_eq(hs->hs_key_url, hv->hv_key_url)) {
      HLS_TRACE(h, "Loading key %s", rstr_get(hs->hs_key_url));
      buf_release(hv->hv_key);
      hv->hv_key = fa_load(rstr_get(hs->hs_key_url),
                            FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)),
                            NULL);
      if(hv->hv_key == NULL) {
	TRACE(TRACE_ERROR, "HLS", "Unable to load key file %s",
	      rstr_get(hs->hs_key_url));
	fa_close(fh);
        return HLS_ERROR_SEGMENT_BAD_KEY;
      }
      rstr_set(&hv->hv_key_url, hs->hs_key_url);
    }

    fh = fa_aescbc_open(fh, hs->hs_iv, buf_c8(hv->hv_key));
  }

  switch(hs->hs_map) {
  case HLS_MAP_FMP4:

    /*if(!rstr_eq(hs->hs_map_url, hv->hv_map_url)) {
      HLS_TRACE(h, "Loading map %s", rstr_get(hs->hs_map_url));
      buf_release(hv->hv_map);
      hv->hv_map = fa_load(rstr_get(hs->hs_map_url),
                            FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)),
                            NULL);
      if(hv->hv_map == NULL) {
	TRACE(TRACE_ERROR, "HLS", "Unable to load map file %s",
	      rstr_get(hs->hs_map_url));
	fa_close(fh);
        return HLS_ERROR_SEGMENT_BROKEN;
      }
	  TRACE(TRACE_ERROR, "HLS", "Map size: %zu", buf_size(hv->hv_map));
	  //hexdump("BUFFER", buf_data(hv->hv_map), 256);
      rstr_set(&hv->hv_map_url, hs->hs_map_url);
    }
	*/
	break;

    //fh = fa_aescbc_open(fh, hs->hs_iv, buf_c8(hv->hv_key));
  }


  hs->hs_fh = fh;
  //HLS_TRACE(h, "Opened %s (sequence %d) ranges:[%d + %d] OK",
  //          hs->hs_url, hs->hs_seq, hs->hs_byte_offset, hs->hs_byte_size);
  return 0;
}


/**
 *
 */
void
hls_segment_close(hls_segment_t *hs)
{
  if(hs->hs_fh == NULL)
    return;

#if 0
  hls_demuxer_t *hd = hs->hs_variant->hv_demuxer;
  hls_t *h = hd->hd_hls;

  if(hs->hs_blocked_counter == h->h_blocked) {
    int64_t ts = arch_get_ts() - hs->hs_open_time;
    if(ts > 1000 && hs->hs_size > 0) {
      int64_t bw = 8000000LL * hs->hs_size / ts;
      bw = MIN(100000000, bw);

      int low_buffer = h->h_mp->mp_buffer_delay < 6000000; //video_settings.video_buffer_size*1000000;//6000000;

      const char *delta;
      if(hd->hd_bw == 0) {
        hd->hd_bw = bw;
        delta = "Initial";
      } else if(bw < hd->hd_bw) {
        delta = "Decrease";
        if(low_buffer)
          hd->hd_bw = (hd->hd_bw + bw) / 2;
        else
          hd->hd_bw = (hd->hd_bw * 7 + bw) / 8;
      } else {
        delta = "Increase";
        hd->hd_bw = (hd->hd_bw + bw) / 2;
      }
      HLS_TRACE(h, "Estimated bandwidth updated %d bps "
                "(most recent segment %d bps) "
                "buffer: %ds (%s) delta: %s\n",
                hd->hd_bw, (int)bw,
                (int)(h->h_mp->mp_buffer_delay / 1000000),
                low_buffer ? "Low" : "OK",
                delta);
      hd->hd_bw_updated = 1;
    }
  }
#endif
  fa_close(hs->hs_fh);
  hs->hs_fh = NULL;
}


/**
 *
 */
static int64_t
hv_find_segment_time_offset(const hls_variant_t *hv, int64_t pos, int hs_dur)
{
  hls_segment_t *hs;
  TAILQ_FOREACH_REVERSE(hs, &hv->hv_segments, hls_segment_queue, hs_link) {
    if(hs->hs_time_offset <= pos) {
      if(hs == TAILQ_LAST(&hv->hv_segments, hls_segment_queue)) {
	if(pos > hs->hs_time_offset + hs->hs_duration)
	  return (hs_dur?hs->hs_duration:-1);
      }
      break;
    }
  }
  if(!hs) return 0;
  if(hs_dur) return hs->hs_duration;
  return hs->hs_time_offset;
}

static hls_segment_t *
hv_find_segment_by_time(const hls_variant_t *hv, int64_t pos)
{
  hls_segment_t *hs;
  TAILQ_FOREACH_REVERSE(hs, &hv->hv_segments, hls_segment_queue, hs_link) {
    if(hs->hs_time_offset <= pos) {
      if(hs == TAILQ_LAST(&hv->hv_segments, hls_segment_queue)) {
	if(pos > hs->hs_time_offset + hs->hs_duration)
	  return NULL;
      }
      break;
    }
  }
  return hs;
}


/**
 *
 */
static int
get_current_video_seq(media_pipe_t *mp, hls_t *h)
{
  int seq = h->h_last_enqueued_seq;
  media_buf_t *mb;

  hts_mutex_lock(&mp->mp_mutex);

  mb = TAILQ_FIRST(&mp->mp_video.mq_q_data);
  if(mb != NULL)
    seq = mb->mb_sequence;
  hts_mutex_unlock(&mp->mp_mutex);
  return seq;
}


/**
 * Most requests are for same sequence so we just keep a cache pointer
 */
hls_segment_t *
hv_find_segment_by_seq(hls_variant_t *hv, int seq)
{
  hls_segment_t *hs = hv->hv_segment_search;
  if(hs != NULL) {
    if(hs->hs_seq == seq)
      return hs;
    hs = TAILQ_NEXT(hs, hs_link);
    if(hs != NULL && hs->hs_seq == seq) {
      hv->hv_segment_search = hs;
      return hs;
    }
  }

  hs = TAILQ_LAST(&hv->hv_segments, hls_segment_queue);
  if(hs == NULL)
    return NULL;
  if(hs->hs_seq == seq)
    return hs;

  TAILQ_FOREACH(hs, &hv->hv_segments, hs_link) {
    if(hs->hs_seq == seq) {
      hv->hv_segment_search = hs;
      break;
    }
  }
  return hs;
}


/**
 *
 */
void
hls_variant_open(hls_variant_t *hv)
{
  hls_demuxer_t *hd = hv->hv_demuxer;
  hls_t *h = hd->hd_hls;

  if(hd == &h->h_primary) {
    media_pipe_t *mp = h->h_mp;
    char info[64];
    if(hv->hv_bitrate) {

      snprintf(info, sizeof(info), "HLS%s %s%dx%d (%d kb/s)", ((mp->mp_hls_source & 3)==2?" fMP4":""), (hv->hv_codec_hdr?"HDR ":""), hv->hv_width, hv->hv_height, hv->hv_bitrate / 1000);
    } else {
		if(hv->hv_width && hv->hv_height)
			snprintf(info, sizeof(info), "HLS%s %s%dx%d", ((mp->mp_hls_source & 3)==2 ?" fMP4":""), (hv->hv_codec_hdr?"HDR ":""), hv->hv_width, hv->hv_height);
		else
			snprintf(info, sizeof(info), "HLS%s %s", ((mp->mp_hls_source & 3)==2 ?" fMP4":""), (hv->hv_codec_hdr?"HDR ":""));
    }
#if defined(__ANDROID__)
	if(hv->hv_width && hv->hv_height)
	{
		h->h_mp->mp_hls_source_width = hv->hv_width;
		h->h_mp->mp_hls_source_height = hv->hv_height;
	}
#endif
    prop_set(mp->mp_prop_metadata, "format", PROP_SET_STRING, info);
#if 0
    prop_t *fmt = prop_create_r(mp->mp_prop_metadata, "format");

    if(hv->hv_bitrate) {
      char info[64];
      snprintf(info, sizeof(info), "HLS %d kb/s", hv->hv_bitrate / 1000);
      mp_send_prop_set_string(mp, &mp->mp_audio, fmt, info);
    } else {
      mp_send_prop_set_string(mp, &mp->mp_audio, fmt, "HLS");
    }

    prop_ref_dec(fmt);
#endif
#if 0 //defined(__ANDROID__)
	if(mp->mp_tunnel_mode && !mp->mp_audio_transcode_delay && (!hv->hv_frozen || mp->mp_hls_source&4) && mp->mp_hls_source&1)
	{
	   mp_set_duration(mp, AV_NOPTS_VALUE);
	   mp_set_clr_flags(mp, 0, MP_CAN_SEEK);
	}
	else
#endif
    //mp_set_duration(mp, hv->hv_frozen ? hv->hv_duration :  AV_NOPTS_VALUE);
    //if(hv->hv_frozen)
	{
      mp_set_clr_flags(mp, MP_CAN_SEEK, 0);
	  mp_set_duration(mp, hv->hv_duration);
	}
	/*
	else
    {
		if(video_settings.hls_live_mode >= 2) {
		  mp_set_clr_flags(mp, MP_CAN_SEEK, 0);
		  mp_set_duration(mp, hv->hv_duration);
		}
		else
		{
		   mp_set_duration(mp, AV_NOPTS_VALUE);
	       mp_set_clr_flags(mp, 0, MP_CAN_SEEK);
		}
    }
	*/
  }
}


/**
 *
 */
void
hls_variant_close(hls_variant_t *hv)
{
  if(hv->hv_demuxer_close != NULL)
    hv->hv_demuxer_close(hv);

  if(hv->hv_current_seg != NULL) {
    hls_segment_close(hv->hv_current_seg);
    hv->hv_current_seg = NULL;
  }
}


/**
 *
 */
hls_segment_t *
hls_variant_select_next_segment(hls_variant_t *hv)
{
  hls_demuxer_t *hd = hv->hv_demuxer;
  hls_t *h = hd->hd_hls;
  media_pipe_t *mp = h->h_mp;
  hls_segment_t *hs;


  if(hv->hv_loaded == 0) {
    hls_error_t err = hls_variant_update(hv, mp);

    if(err != HLS_ERROR_OK) {
      hls_bad_variant(hv, err);
      return NULL;
    }
  }
  else
  //if(video_settings.hls_live_mode>=2)
  {
    if( (time(NULL) - hv->hv_loaded) > 8 ) {
	  //TRACE(TRACE_ERROR, "HLS", "Diff: %li", (time(NULL)-hv->hv_loaded));
      hls_error_t err = hls_variant_update(hv, mp);
      if(err != HLS_ERROR_OK)
	  {
		TRACE(TRACE_ERROR, "HLS", "Variand update failed");
	  }
    }
  }

  int attempts = 0;
 retry:
  if(hv->hv_current_seg == NULL) {

    hs = NULL;
    if(hd->hd_seek_to_segment != PTS_UNSET) {
      hs = hv_find_segment_by_time(hv, hd->hd_seek_to_segment);

      if(hv->hv_frozen && hs == NULL)
	  {
		//TRACE(TRACE_ERROR, "HLS", "hv->hv_frozen=%i hs=NULL=%i", hv->hv_frozen, (hs==NULL));
        return HLS_EOF;
	  }

      if(hs != NULL) {
        HLS_TRACE(h, "%s: Seek to %"PRId64" -- Segment %d", hd->hd_type,
                  hd->hd_seek_to_segment, hs->hs_seq);
      } else {
        HLS_TRACE(h, "%s: Seek to %"PRId64" -- Segment not found", hd->hd_type,
                  hd->hd_seek_to_segment);
      }

      hd->hd_seek_to_segment = PTS_UNSET;
    }

    if(hs == NULL) {
      int seq = get_current_video_seq(h->h_mp, h);
      if(/*seq == 0 &&*/ !hv->hv_frozen) {

		if(hv->hv_start_time_offset != PTS_UNSET)
		{
			int64_t acc = 0;
			int64_t total_dur = 0;

			if( hv->hv_start_time_offset < 0 )
			{
				hls_segment_t *xt;
				TAILQ_FOREACH(xt, &hv->hv_segments, hs_link)
				{
					total_dur += xt->hs_duration;
				}

				//TRACE(TRACE_ERROR, "HLS", "hv->hv_start_time_offset=%lld", hv->hv_start_time_offset);
				//TRACE(TRACE_ERROR, "HLS", "total_dur=%lld", total_dur);
				TAILQ_FOREACH(xt, &hv->hv_segments, hs_link)
				{
					acc += xt->hs_duration;
					if( (total_dur + hv->hv_start_time_offset) < acc)
					{
					  hs = xt;
					  HLS_TRACE(h, "Live stream starting at segment %d, position %.2f sec (%.2f sec from live edge)", hs->hs_seq, (acc-xt->hs_duration)/1000000.f, hv->hv_start_time_offset/1000000.f);
					  break;
					}
				}
			}
			else
			{
				hls_segment_t *x;
				TAILQ_FOREACH(x, &hv->hv_segments, hs_link)
				{
					acc += x->hs_duration;
					if(hv->hv_start_time_offset < acc)
					{
					  hs = x;
					  HLS_TRACE(h, "Live stream starting at segment %d", hs->hs_seq);
					  break;
					}
				}
			}
        }

        if(hs == NULL) {
			seq = MAX(hv->hv_last_seq - 18, hv->hv_first_seq);
		  /*
			if(video_settings.hls_live_mode == 0)// || video_settings.hls_live_mode == 3)
	          seq = MAX(hv->hv_last_seq - 18, hv->hv_first_seq);
			else
			if(video_settings.hls_live_mode == 1)
	          seq = MAX(hv->hv_last_seq - 35, hv->hv_first_seq);
			else
			//if(video_settings.hls_live_mode == 2)
	          seq = hv->hv_first_seq;
		  */
          HLS_TRACE(h, "Live stream selecting initial segment %d", seq);
        }
      }

      if(hs == NULL && seq) {

        int i;
		int s_step = 18;
		//if(video_settings.hls_live_mode == 1) s_step = 35;
        for(i = 0; i < s_step; i++) {
          hs = hv_find_segment_by_seq(hv, seq + i);
          if(hs != NULL)
            break;
          HLS_TRACE(h, "Lookup of seq %d failed, trying next", seq + i);
        }
      }
    }
    if(hs == NULL)
      hs = TAILQ_FIRST(&hv->hv_segments);

  } else {
    hs = TAILQ_NEXT(hv->hv_current_seg, hs_link);
  }

#if defined(__ANDROID__)
	  if(strstr(hd->hd_type, "primary"))
	  {
		  //mp->mp_seeking = 0;
		  //TRACE(TRACE_INFO, "HLS", "SEEKING END");
	  }
#endif

  if(hs != NULL)
    return hs;

  if(hv->hv_frozen) {
    // We're not a live stream and no segment can be found, this is EOF
	//TRACE(TRACE_ERROR, "HLS", "hls_variant_select_next_segment() - hv->hv_frozen=%i hs=NULL=%i", hv->hv_frozen, (hs==NULL));
    return HLS_EOF;
  }

  if(  mp->mp_buffer_delay < 15000000 ||
	  (mp->mp_buffer_delay >=30000000 && ((time(NULL) - hv->hv_loaded) > 8)) // && video_settings.hls_live_mode >= 2
	  )
	{

	if( (time(NULL) - hv->hv_loaded) > 3 )
    {
    //if(hv->hv_loaded != time(NULL))
      hls_error_t err = hls_variant_update(hv, mp);
		//TRACE(TRACE_INFO, "HLS", "hls_variant_update() attempts=%i (%li seconds)", attempts, (time(NULL) - hv->hv_loaded));
      if(err != HLS_ERROR_OK)
	  {
		  //TRACE(TRACE_ERROR, "HLS", "hls_variant_update() err=%i attempts=%i", err, attempts);
		attempts++;
		if(attempts<14)
		{
	        usleep(750000);
			goto retry;
		}
        hls_bad_variant(hv, err);
        //return NULL;

		return HLS_EOF;

      }
	  attempts = 0;
      goto retry;
    }
  }

  /*
   * Ok, so no segment is availble yet => go to sleep (but wakeup
   * if we need to exit or if we need to seek someplace)
   */

  hts_mutex_lock(&mp->mp_mutex);

  if(TAILQ_FIRST(&mp->mp_eq) == NULL) {

    if(hts_cond_wait_timeout(&mp->mp_backpressure, &mp->mp_mutex, 1000)) {
      hts_mutex_unlock(&mp->mp_mutex);
      return HLS_NYA;
    }
  }
  hts_mutex_unlock(&mp->mp_mutex);
  return NULL;
}

#if defined(__ANDROID__)
void
hls_get_max_width_height(hls_demuxer_t *hd, int *wd, int *hg)
{
  hls_variant_t *hv;

  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
	  if(!hv->hv_audio_only)
	  {
		if(
			//SD
			((video_settings.hls_limit == 1 || video_settings.hls_limit ==  7 || video_settings.hls_limit ==  13) && !(hv->hv_width >  gconf.hls_limit_sd || hv->hv_height > 480) ) ||
			//720p
			((video_settings.hls_limit == 2 || video_settings.hls_limit ==  8 || video_settings.hls_limit ==  14) && !(hv->hv_width > 1280 || hv->hv_height > 720) ) ||
			//1080p
			((video_settings.hls_limit == 3 || video_settings.hls_limit ==  9 || video_settings.hls_limit ==  15) && !(hv->hv_width > 1920 || hv->hv_height > 1080) ) ||
			//1440p
			((video_settings.hls_limit == 4 || video_settings.hls_limit == 10 || video_settings.hls_limit ==  16) && !(hv->hv_width > 2560 || hv->hv_height > 1440) ) ||
			//4K
			((video_settings.hls_limit == 5 || video_settings.hls_limit == 11 || video_settings.hls_limit ==  17) && !(hv->hv_width > 3840 || hv->hv_height > 2160) ) ||
			//AVC ANY / AVC/HEVC ANY
			((video_settings.hls_limit == 6)) || ((video_settings.hls_limit == 12)) ||
			//No Limit
			!video_settings.hls_limit
		)
		{
			if( hv->hv_width > *wd ) *wd = hv->hv_width;
			if( hv->hv_height > *hg ) *hg = hv->hv_height;
		}
	  }
  }
}
#endif

/**
 * Select a low bitrate variant that we hope works ok
 */
hls_variant_t *
hls_select_default_variant(hls_demuxer_t *hd)
{
  hls_variant_t *hv;
  hls_variant_t *best = NULL;
  hls_variant_t *worst = NULL;
  int bw = 0;
  int wd = 0;

  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
	  if(!hv->hv_audio_only)
	  {
		  if( (hv->hv_width > wd) || (hv->hv_width == wd && hv->hv_bitrate < bw) )
		  {
			bw = hv->hv_bitrate;
			wd = hv->hv_width;
		  }
	  }
  }

  int hv_default_set = 0;

  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {

	/*
	if(!video_settings.hls_best_quality)
	{
      if(hv->hv_initial)
      return hv;
	}
	else
		if(!hv->hv_audio_only && hv->hv_bitrate > bw) bw = hv->hv_bitrate;
	*/

	if(last_bw > 0)
	{
		//TRACE(TRACE_DEBUG, "HLS", "Checking variant: %ix%i (%i bps%s)", hv->hv_width, hv->hv_height, (int)(hv->hv_bitrate), hv->hv_codec_avc?" AVC":"");

		if(hv->hv_bitrate == last_bw)
		{
			TRACE(TRACE_DEBUG, "HLS", "Selecting variant: %ix%i (%i kb/s%s)", hv->hv_width, hv->hv_height, (int)(hv->hv_bitrate/1000), hv->hv_codec==AV_CODEC_ID_H264?" AVC":"");
			last_bw = -1;
			return hv;
		}
	}

	if(video_settings.hls_limit && hv->hv_width>128 && hv->hv_height>64)
	{
		if(
			//SD
			((video_settings.hls_limit == 1 || video_settings.hls_limit ==  7 || video_settings.hls_limit ==  13) && !(hv->hv_width >  gconf.hls_limit_sd || hv->hv_height > 480) ) ||
			//720p
			((video_settings.hls_limit == 2 || video_settings.hls_limit ==  8 || video_settings.hls_limit ==  14) && !(hv->hv_width > 1280 || hv->hv_height > 720) ) ||
			//1080p
			((video_settings.hls_limit == 3 || video_settings.hls_limit ==  9 || video_settings.hls_limit ==  15) && !(hv->hv_width > 1920 || hv->hv_height > 1080) ) ||
			//1440p
			((video_settings.hls_limit == 4 || video_settings.hls_limit == 10 || video_settings.hls_limit ==  16) && !(hv->hv_width > 2560 || hv->hv_height > 1440) ) ||
			//4K
			((video_settings.hls_limit == 5 || video_settings.hls_limit == 11 || video_settings.hls_limit ==  17) && !(hv->hv_width > 3840 || hv->hv_height > 2160) ) ||
			//AVC ANY / AVC/HEVC ANY
			((video_settings.hls_limit == 6)) || ((video_settings.hls_limit == 12))
		)
		{
			if( best == NULL && ((hv->hv_codec==AV_CODEC_ID_H264 && video_settings.hls_limit >=  6) || (video_settings.hls_limit <= 5)) ) best = hv;
			if( best == NULL && ((hv->hv_codec!=AV_CODEC_ID_AV1  && video_settings.hls_limit >= 12) || (video_settings.hls_limit <= 5)) ) best = hv;
			if( best != NULL && (hv->hv_default || (hv->hv_width > best->hv_width || hv->hv_height > best->hv_height) || (hv->hv_width == best->hv_width && hv->hv_bitrate < best->hv_bitrate)) )
			{
				if(
					( (hv->hv_codec==AV_CODEC_ID_H264 && video_settings.hls_limit >=  6 && video_settings.hls_limit <= 17) || (video_settings.hls_limit <= 5) ) ||
					( (hv->hv_codec!=AV_CODEC_ID_AV1  && video_settings.hls_limit >= 12 && video_settings.hls_limit <= 17) || (video_settings.hls_limit <= 5) )
				)
				{
					//TRACE(TRACE_DEBUG, "HLS", "Last best variant so far: %ix%i avc:%i", best->hv_width, best->hv_height, best->hv_codec_avc);
					//TRACE(TRACE_DEBUG, "HLS", "Best variant so far: %ix%i avc:%i", hv->hv_width, hv->hv_height, hv->hv_codec_avc);
					if(hv->hv_default || (!hv->hv_default && !hv_default_set))
						best = hv;
					if(hv->hv_default) hv_default_set = 1;
				}
			}
		}
		else
		{
			if( worst == NULL && ((hv->hv_codec==AV_CODEC_ID_H264 && video_settings.hls_limit >=  6) || (video_settings.hls_limit <= 5)) ) worst = hv;
			if( worst == NULL && ((hv->hv_codec!=AV_CODEC_ID_AV1  && video_settings.hls_limit >= 12) || (video_settings.hls_limit <= 5)) ) worst = hv;
			if( worst != NULL && ((hv->hv_width < worst->hv_width || hv->hv_height < worst->hv_height) || (hv->hv_width == worst->hv_width && hv->hv_bitrate < worst->hv_bitrate)) )
			{
				if(
					( (hv->hv_codec==AV_CODEC_ID_H264 && video_settings.hls_limit >=  6 && video_settings.hls_limit <= 17) || (video_settings.hls_limit <= 5) ) ||
					( (hv->hv_codec!=AV_CODEC_ID_AV1  && video_settings.hls_limit >= 12 && video_settings.hls_limit <= 17) || (video_settings.hls_limit <= 5) )
				)
				{
					//TRACE(TRACE_DEBUG, "HLS", "Last worst variant so far: %ix%i avc:%i", worst->hv_width, worst->hv_height, worst->hv_codec_avc);
					//TRACE(TRACE_DEBUG, "HLS", "Worst variant so far: %ix%i avc:%i", hv->hv_width, hv->hv_height, hv->hv_codec_avc);
					worst = hv;
				}
			}
		}
	}
  }

  last_bw = -1;

  if(video_settings.hls_limit)
  {
	  if(best != NULL)
	  {
		TRACE(TRACE_DEBUG, "HLS", "Selecting initial variant: %ix%i (%i kb/s%s)", best->hv_width, best->hv_height, (int)(best->hv_bitrate/1000), best->hv_codec==AV_CODEC_ID_H264?" AVC":"");
		return best;
	  }

	  if(worst != NULL)
	  {
		TRACE(TRACE_ERROR, "HLS", "Selecting initial variant: %ix%i (%i kb/s%s) - all variants exceed configured limit", worst->hv_width, worst->hv_height, (int)(worst->hv_bitrate/1000), worst->hv_codec==AV_CODEC_ID_H264?" AVC":"");
		return worst;
	  }

	  if(hv != NULL)
	  {
		TRACE(TRACE_ERROR, "HLS", "Selecting default variant: %ix%i (%i kb/s%s) - all variants exceed configured limit", hv->hv_width, hv->hv_height, (int)(hv->hv_bitrate/1000), hv->hv_codec==AV_CODEC_ID_H264?" AVC":"");
		return hv;
	  }
  }

	  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {

		if(hv->hv_default)
		  return hv;
	  }

	  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {

		if(bw && hv->hv_bitrate == bw)
		  return hv;
	  }


  TAILQ_FOREACH_REVERSE(hv, &hd->hd_variants, hls_variant_queue, hv_link) {
    if(hv->hv_audio_only)
      continue;

    if(best == NULL || best->hv_corrupt_counter > hv->hv_corrupt_counter)
      best = hv;
  }
  return best;
}



/**
 *
 */
static hls_variant_t *
demuxer_select_variant_simple(hls_demuxer_t *hd, int64_t now, int bw)
{
  hls_variant_t *hv;
  hls_variant_t *best = NULL;

  int lcc = INT32_MAX;

  // Figure out which stream have the lowest corruption counter
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    if(hv->hv_audio_only)
      continue;
    if(hv->hv_corrupt_timer < now - HLS_CORRUPTION_MEASURE_PERIOD)
      hv->hv_corruptions_last_period = 0;

    lcc = MIN(lcc, hv->hv_corrupt_counter);
  }

  // First, try to select best mathcing bandwidth among streams with
  // lowest corruption

  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
	  //TRACE(TRACE_DEBUG, "BW-SW", "Variant BW=%i (REQ:%i)", hv->hv_bitrate, bw);
    if(hv->hv_audio_only)
      continue;

	if(last_bw > 0 && hv->hv_bitrate == last_bw)
	{
		best = hv;
		last_bw = -1;
		break;
	}

    if(hv->hv_corruptions_last_period >= 3)
      continue;

    if(hv->hv_corrupt_counter != lcc)
      continue;

    if(hv->hv_bitrate <= bw)
      continue;

    if(best == NULL)
      best = hv;
  }

  if(best != NULL)
    return best;

  // Try to select something that's not corrupted

  TAILQ_FOREACH_REVERSE(hv, &hd->hd_variants, hls_variant_queue, hv_link) {
    if(hv->hv_audio_only)
      continue;
    if(hv->hv_corruptions_last_period >= 3)
      continue;

    return hv;
  }
  hd->hd_no_functional_streams = 1;
  return NULL;
}


#if 0
/**
 *
 */
static hls_variant_t *
demuxer_select_variant_random(hls_demuxer_t *hd)
{
  hls_variant_t *hv;
  int cnt = 0;

  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    if(!hv->hv_audio_only)
      cnt++;

  }

  int r = rand() % cnt;
  cnt = 0;
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    if(hv->hv_audio_only)
      continue;
    if(r == cnt)
      break;
    cnt++;
  }

  HLS_TRACE(hd->hd_hls, "Randomly selected bitrate %d", hv ? hv->hv_bitrate : 0);
  return hv;
}
#endif
/**
 *
 */
static hls_variant_t *
hls_demuxer_select_variant(hls_demuxer_t *hd, int64_t now, int bw)
{
  //if(0)
  //  return demuxer_select_variant_random(hd);

  return demuxer_select_variant_simple(hd, now, bw);
}

/*
int
hls_bw_switch(hls_demuxer_t *hd, int bw)
{

	hls_t *h = hd->hd_hls;
	media_pipe_t *mp = h->h_mp;

	hls_variant_t *hv;
	//hv = hls_demuxer_select_variant(hd, now, hd->hd_bw/3);

	TAILQ_FOREACH(hv, &hd->hd_variants, hv_link)
	{
		TRACE(TRACE_DEBUG, "BW-SW", "Variant BW=%i (REQ:%i)", hv->hv_bitrate, bw);
		if(hv->hv_audio_only)
			continue;

		if(hv->hv_bitrate == bw)
		{
			//hd->hd_current = hv;
			hd->hd_req = hv;
			hls_free_mbp(mp, &hd->hd_mb);
			last_bw = 0;
			return 1;
		}
	}
	last_bw = 0;
	return 0;
}
*/

/**
 *
 */
#if 0
void
hls_check_bw_switch(hls_demuxer_t *hd)
{

  hls_t *h = hd->hd_hls;
  media_pipe_t *mp = h->h_mp;

  if(hd->hd_bw_updated == 0 || video_settings.hls_best_quality)
    return;

  if(hd == &h->h_primary) {
    prop_set(mp->mp_prop_io, "bitrate", PROP_SET_INT, hd->hd_bw / 1000);
    prop_set(mp->mp_prop_io, "bitrateValid", PROP_SET_INT, 1);
  }

  int64_t now = arch_get_ts();
  if(hd->hd_last_switch + 10000000 > now)
    return;

  hd->hd_bw_updated = 0;
  hls_variant_t *hv = NULL;

//TRACE(TRACE_DEBUG, "HLS", "   INI     hv_bitrate=%i  hd_bw=%i ", hd->hd_current->hv_bitrate, hd->hd_bw);
  if(hd->hd_current->hv_bitrate > hd->hd_bw)
	  hv = hls_demuxer_select_variant(hd, now, hd->hd_bw/3);
  else
	hv = hls_demuxer_select_variant(hd, now, hd->hd_bw);

//TRACE(TRACE_DEBUG, "HLS", "   END     hv_bitrate=%i  hd_bw=%i ", hd->hd_current->hv_bitrate, hd->hd_bw);

  if(hv == NULL || hv == hd->hd_current)
  {
	hd->hd_last_switch = now;
    return;
  }


  if(hv->hv_bitrate > hd->hd_current->hv_bitrate && hv->hv_bitrate <= hd->hd_bw) {
    // Stepping up, only do it if we have more than 20s worth of buffer
    if(mp->mp_buffer_delay < 20000000) {
      hd->hd_last_switch = now;
      return;
    }
  }

	if(hv->hv_bitrate > hd->hd_current->hv_bitrate && hv->hv_bitrate <= hd->hd_bw)
	{
		//TRACE(TRACE_DEBUG, "HLS", "   SWC  to hv_bitrate=%i  hd_bw=%i ", hv->hv_bitrate, hd->hd_bw);
		hd->hd_last_switch = now;
		hd->hd_req = hv;

		hls_free_mbp(mp, &hd->hd_mb);
	}
}
#endif

/**
 *
 */
static int
hls_event_callback(media_pipe_t *mp, void *aux, event_t *e)
{
  hls_t *h = aux;

  if(event_is_type(e, EVENT_CURRENT_TIME)) {

    event_ts_t *ets = (event_ts_t *)e;

    if(ets->epoch == mp->mp_epoch)
	{
      int sec = ets->ts / 1000000;
      h->h_last_timestamp_presented = ets->ts;

		// Update restartpos every 5 seconds
		if((sec < h->h_restartpos_last) || (sec >= (h->h_restartpos_last + 5)))
		{
			if(h->h_restartpos_last != -1)
				prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 0);

			h->h_restartpos_last = sec;

			mp->mp_buffer_limit = video_settings.video_buffer_size * 1024 * 1024;
			prop_set_int(mp->mp_prop_buffer_limit, mp->mp_buffer_limit);

			//if(h->h_primary.hd_current->hv_frozen)
			//	playinfo_set_restartpos(canonical_url, ets->ts / 1000, 1);
		}
	}

  } else if(event_is_type(e, EVENT_PLAYBACK_PRIORITY)) {
    event_int_t *ei = (event_int_t *)e;
    h->h_playback_priority = ei->val;

  } else if(event_is_type(e, EVENT_SELECT_AUDIO_TRACK)) {
    event_select_track_t *est = (event_select_track_t *)e;
    const char *id = mystrbegins(est->id, "hls:");
    if(id != NULL) {
      const int streamid = atoi(id);
      assert(streamid != 0);
      h->h_audio.hd_pending_stream = streamid;
	  if((mp->mp_hls_source & 3) == 2) mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
    }
	else
	{
		const char *idv = mystrbegins(est->id, "hlsv:");
		if(idv != NULL)
		{
			const int streamid = atoi(idv);
			if(streamid > 1024 && streamid < 100000000)
			{
				last_bw = streamid;
				last_pos = h->h_last_timestamp_presented;

				//TRACE(TRACE_INFO, "HLS", "last_pos=%lli", last_pos);
				//usleep(500000);
				//TRACE(TRACE_INFO, "HLS", "last_pos=%lli (sleep done)", last_pos);
				//last_bw = 3499968;
				//last_bw = 920615;
				//last_bw = 1057716;
				//hls_variant_close(h->h_primary.hd_current);

				//h->h_primary.hd_req = hls_select_default_variant(&h->h_primary);
				//mp_event_dispatch(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));

#if 0
				if((mp->mp_hls_source & 3) == 2) // fMP4
					mp_enqueue_event_locked(mp, event_create_action(ACTION_RELOAD_DATA));
				else
				{
					h->h_primary.hd_req = hls_select_default_variant(&h->h_primary);
					mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				}

#else
				hls_variant_t *hv_tmp;
				hv_tmp = hls_select_default_variant(&h->h_primary);

				if(hv_tmp != NULL)
				{
#if defined(__ANDROID__)
					//TRACE(TRACE_INFO, "HLS", "current map=%s", rstr_get(h->h_primary.hd_current->hv_map_url));
					//TRACE(TRACE_INFO, "HLS", "current fps=%.3f codec=%i hdr=%i", h->h_primary.hd_current->hv_fps, h->h_primary.hd_current->hv_codec, h->h_primary.hd_current->hv_codec_hdr);
					//TRACE(TRACE_INFO, "HLS", "new     fps=%.3f codec=%i hdr=%i", hv_tmp->hv_fps, hv_tmp->hv_codec, hv_tmp->hv_codec_hdr);
					if( //(hv_tmp->hv_codec == AV_CODEC_ID_AV1 || (!(mp->mp_hls_source & 2)))
						//&& h->h_primary.hd_current->hv_fps == hv_tmp->hv_fps
						//&&
						h->h_primary.hd_current->hv_codec == hv_tmp->hv_codec
						//&& h->h_primary.hd_current->hv_codec_hdr == hv_tmp->hv_codec_hdr
						//&& (h->h_primary.hd_current->hv_map_url==NULL || ((strstr(rstr_get(h->h_primary.hd_current->hv_map_url), "googlevideo")==NULL && strstr(rstr_get(h->h_primary.hd_current->hv_map_url), "nebula.tv")==NULL) || hv_tmp->hv_codec==AV_CODEC_ID_AV1))
						)
					{
						h->h_primary.hd_req = hv_tmp;
						//mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
						//mp_enqueue_event_locked(mp, event_create_action(ACTION_PAUSE));
						//mp_enqueue_event_locked(mp, event_create_action(ACTION_PLAY));
						last_bw = -1;
						last_pos = -1;
						if((mp->mp_hls_source & 3) != 2)
						{
							mp_enqueue_event_locked(mp, event_create_action(ACTION_PAUSE));
							mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
							mp_enqueue_event_locked(mp, event_create_action(ACTION_PLAY));
						}
					}
					else
#endif
					{
						last_bw = streamid;
						last_pos = h->h_last_timestamp_presented;
						if((mp->mp_hls_source & 3) == 2)
						{
							mp_enqueue_event_locked(mp, event_create_action(ACTION_PAUSE));
							//mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
							usleep(333333);
						}
						mp_enqueue_event_locked(mp, event_create_action(ACTION_RELOAD_DATA));
					}
				}
#endif
				/*
				cancellable_cancel(h->h_primary.hd_cancellable);
				cancellable_cancel(h->h_audio.hd_cancellable);
				cancellable_cancel(h->h_subtitle.hd_cancellable);
				return 1;
				*/
			}
		}
	}
  } else if(event_is_type(e, EVENT_SELECT_SUBTITLE_TRACK)) {
    event_select_track_t *est = (event_select_track_t *)e;
    const char *id = mystrbegins(est->id, "hls:");
    if(id != NULL) {
      const int streamid = atoi(id);
      assert(streamid != 0);
      h->h_subtitle.hd_pending_stream = streamid;
    }
  } else if(event_is_action(e, ACTION_SKIP_FORWARD) ||
            event_is_action(e, ACTION_SKIP_BACKWARD) ||
#if defined(__ANDROID__)
			event_is_action(e, ACTION_RELOAD_REPLAY) ||
#endif
			event_is_action(e, ACTION_RELOAD_DATA) ||
            event_is_type(e, EVENT_EXIT) ||
            event_is_type(e, EVENT_PLAY_URL) ||
            event_is_type(e, EVENT_SEEK)) {

    cancellable_cancel(h->h_primary.hd_cancellable);
    cancellable_cancel(h->h_audio.hd_cancellable);
	cancellable_cancel(h->h_subtitle.hd_cancellable);
    return 0; // Continue processing
  }

  return 1;
}


/**
 *
 */
static void
extract_ps_nal(hls_variant_t *hv, const uint8_t *data, int len)
{
  if(len == 0)
    return;

  const int nal_unit_type = data[0] & 0x1f;

  if(nal_unit_type == 7 /*SPS*/ || nal_unit_type == 8 /*SEI*/)
  {
    int newsize = len + 3 + hv->hv_video_headers_size;
    hv->hv_video_headers = realloc(hv->hv_video_headers, newsize);

    memcpy(hv->hv_video_headers + hv->hv_video_headers_size,
           data - 3, len + 3);
    hv->hv_video_headers_size = newsize;
  }
}


/**
 *
 */
static void
extract_ps(hls_variant_t *hv, media_buf_t *mb)
{
  const uint8_t *d = mb->mb_data;
  int len = mb->mb_size;

  const uint8_t *p = NULL;

  while(len > 3) {
    if(!(d[0] == 0 && d[1] == 0 && d[2] == 1)) {
      d++;
      len--;
      continue;
    }

    if(p != NULL)
      extract_ps_nal(hv, p, d - p);

    d += 3;
    len -= 3;
    p = d;
  }
  d += len;

  if(p != NULL)
    extract_ps_nal(hv, p, d - p);
}


// #define DUMP_VIDEO

#ifdef DUMP_VIDEO

#include <fcntl.h>
/**
 *
 */
static void
dump_video(media_buf_t *mb)
{
  static int fd = -2;
  if(fd == -1)
    return;

  if(fd == -2)
    fd = open("/tmp/videodump.h264", O_TRUNC | O_WRONLY | O_CREAT, 0666);

  if(fd == -1)
    return;

  if(write(fd, mb->mb_data, mb->mb_size) != mb->mb_size) {
    close(fd);
    fd = -1;
  }
}
#endif



/**
 *
 */
static void
drop_early_packets(media_pipe_t *mp, int64_t dts)
{
  media_queue_t *mq = &mp->mp_audio;
  media_buf_t *mb;

  while((mb = TAILQ_FIRST(&mq->mq_q_data)) != NULL) {
    TAILQ_REMOVE(&mq->mq_q_data, mb, mb_link);
    media_buf_free_locked(mp, mb);
  }


  mq = &mp->mp_subtitle;

  while((mb = TAILQ_FIRST(&mq->mq_q_data)) != NULL) {
    TAILQ_REMOVE(&mq->mq_q_data, mb, mb_link);
    media_buf_free_locked(mp, mb);
  }

}


/**
 *
 */
static event_t *
enqueue_buffer(media_pipe_t *mp, media_queue_t *mq, media_buf_t *mb,
               hls_t *h, hls_variant_t *hv)
{
  const int is_video = mb->mb_data_type == MB_VIDEO;

  hts_mutex_lock(&mp->mp_mutex);

  mp_update_buffer_delay(mp);

  while(1) {

    event_t *e = TAILQ_FIRST(&mp->mp_eq);
    if(e != NULL) {
      TAILQ_REMOVE(&mp->mp_eq, e, e_link);
      hts_mutex_unlock(&mp->mp_mutex);
      return e;
    }

    // Check if buffer is full
    //if( mp->mp_buffer_current + mb_buffered_size(mb) < mp->mp_buffer_limit ) break;

#if defined(__ANDROID__)
	if( ( (hv->hv_frozen && mp->mp_buffer_delay<120500000) || (!hv->hv_frozen && mp->mp_buffer_delay<210500000) ) && ((gconf.android_free_mem - mb->mb_size) >= 80*1024*1024 || (mp->mp_buffer_current < 16*1024*1024)) )
#endif
	{
		if( (mp->mp_buffer_current + mb->mb_size /*mb_buffered_size(mb)*/) <= MAX(48*1024*1024, (mp->mp_buffer_limit)) )
		{
#if defined(__ANDROID__)
			//if(f_in_m7_hw_stop && mp->mp_buffer_delay>60000000) { usleep(2000); }
#endif
			break;
		}
	}

    h->h_blocked++;
    hts_cond_wait_timeout(&mp->mp_backpressure, &mp->mp_mutex, 5);
  }

	if(mq->mq_seektarget != AV_NOPTS_VALUE)
	{
		int64_t ts = mb->mb_user_time;
		if(ts < mq->mq_seektarget)
		{
			mb->mb_skip = 1;
			//TRACE(TRACE_DEBUG, "HLS", "mb_skip=1 (%i)", (int)(ts-mq->mq_seektarget)/1000);
		}
		else
			mq->mq_seektarget = AV_NOPTS_VALUE;
	}


	int flush = 0;

  //if(hv->hv_map) goto enq_mb;

  if(mq->mq_demuxer_flags & HLS_QUEUE_MERGE) {

    if(mq->mq_last_deq_dts != PTS_UNSET &&
       mb->mb_dts < mq->mq_last_deq_dts) {
      /*
       * This frame has already been dequeued (DTS lower)
       * Drop packet and restart keyframe search
       */
	   //TRACE(TRACE_DEBUG, "HLS", "---- drop dequeued packet DTS: %lli", mb->mb_dts);

      if(is_video)// && !hv->hv_map_url)
        extract_ps(hv, mb);

      media_buf_free_locked(mp, mb);
      mq->mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
      hts_mutex_unlock(&mp->mp_mutex);
      return NULL;
    }

    media_buf_t *b;
    TAILQ_FOREACH(b, &mq->mq_q_data, mb_link)
      if(b->mb_dts != PTS_UNSET && b->mb_dts >= mb->mb_dts)
        break;

    while(b != NULL) {
      media_buf_t *next = TAILQ_NEXT(b, mb_link);
	  //TRACE(TRACE_DEBUG, "HLS", "---- drop packet DTS: %lli", b->mb_dts);
      if(b->mb_data_type == MB_AUDIO || b->mb_data_type == MB_VIDEO) {
		TAILQ_REMOVE(&mq->mq_q_data, b, mb_link);
        mq->mq_packets_current--;
        mp->mp_buffer_current -= b->mb_size; //mb_buffered_size(b);
        media_buf_free_locked(mp, b);
      }
      b = next;
    }
	if(mb->mb_keyframe)
	{
		mq->mq_demuxer_flags &= ~HLS_QUEUE_MERGE;
		flush = 1;
		//TRACE(TRACE_DEBUG, "HLS", "---- drop packet flush: %i", flush); flush=0;
		//assert(mb->mb_keyframe == 1);
		HLS_TRACE(h, "%s queue merged at DTS %"PRId64,
				  is_video ? "Video" : (mb->mb_data_type == MB_AUDIO ? "Audio" : "Subtitle"), mb->mb_dts);
	}
  }

  if(mb->mb_keyframe)
  {
    mq->mq_demuxer_flags |= HLS_QUEUE_KEYFRAME_SEEN;

	//if(mb->mb_keyframe == 2)
	{
		mb->mb_keyframe &= 1;
		//TRACE(TRACE_DEBUG, "HLS", "mb->mb_keyframe == 2 --> 0 = %i", mb->mb_keyframe);
	}
  }

  if(is_video && !(mq->mq_demuxer_flags & HLS_QUEUE_KEYFRAME_SEEN) &&
     TAILQ_FIRST(&mq->mq_q_data) == NULL) {
    assert(mb->mb_dts != PTS_UNSET);
    drop_early_packets(mp, mb->mb_dts);
  }

  if(mp->mp_hold_flags & MP_HOLD_SYNC &&
     mp->mp_video.mq_demuxer_flags & HLS_QUEUE_KEYFRAME_SEEN &&
     mp->mp_audio.mq_demuxer_flags & HLS_QUEUE_KEYFRAME_SEEN) {
    mp->mp_hold_flags &= ~MP_HOLD_SYNC;
    mp_set_playstatus_by_hold_locked(mp, NULL);
  }


  if(is_video && hv->hv_video_headers_size) {
	  //TRACE(TRACE_DEBUG, "HLS", "hv->hv_video_headers_size=%i",hv->hv_video_headers_size);

    media_buf_t *mbx =
      media_buf_alloc_locked(mp, hv->hv_video_headers_size + mb->mb_size);

    memcpy(mbx->mb_data, hv->hv_video_headers, hv->hv_video_headers_size);
    memcpy(mbx->mb_data + hv->hv_video_headers_size, mb->mb_data, mb->mb_size);

    mbx->mb_keyframe = mb->mb_keyframe;
    mbx->mb_data_type = MB_VIDEO;
    mbx->mb_cw = media_codec_ref(mb->mb_cw);
    mbx->mb_skip = mb->mb_skip;
    mbx->mb_user_time = mb->mb_user_time;

#ifdef DUMP_VIDEO
    dump_video(mbx);
#endif
    mbx->mb_flush = flush;
    mb_enq(mp, mq, mbx);

    flush = 0;
    free(hv->hv_video_headers);
    hv->hv_video_headers = NULL;
    hv->hv_video_headers_size = 0;

    media_buf_free_locked(mp, mb);

    hts_mutex_unlock(&mp->mp_mutex);
    return NULL;
  }

#ifdef DUMP_VIDEO
  if(is_video)
    dump_video(mb);
#endif

//enq_mb:

  mb->mb_flush = flush;

  mb_enq(mp, mq, mb);
  h->h_last_enqueued_seq = mb->mb_sequence;

  if(!is_video && mb->mb_data_type == MB_AUDIO && mq->mq_stream != h->h_audio.hd_current_stream) {
    HLS_TRACE(h, "Telling audio decoder to switch from stream %d to %d",
              mq->mq_stream, h->h_audio.hd_current_stream);
    mq->mq_stream = h->h_audio.hd_current_stream;
  }

  hts_mutex_unlock(&mp->mp_mutex);
  return NULL;
}


/**
 *
 */
static media_buf_t *
hls_demuxer_get(hls_demuxer_t *hd)
{
	if(hd->hd_mb == NULL)
		hd->hd_mb = hls_ts_demuxer_read(hd);

	return hd->hd_mb;
}

/**
 *
 */
static media_buf_t *
hls_demuxer_get_subtitle(hls_t *h)
{
  hls_demuxer_t *hd = &h->h_subtitle;

  if(hd->hd_current_stream != hd->hd_pending_stream) {

    hd->hd_current_stream = hd->hd_pending_stream;

    hls_variant_t *hv;
    TAILQ_FOREACH(hv, &hd->hd_variants, hv_link)
      if(hv->hv_subtitle_stream == hd->hd_current_stream)
        break;

    HLS_TRACE(h, "Subtitle demuxer checking stream switch to streamid: %d -> %s",
              hd->hd_current_stream,
              hv ? hv->hv_name : "<muxed in primary>");

    if(hd->hd_current != NULL) {
      hls_variant_close(hd->hd_current);
    }



    if(hd->hd_current != NULL && hv == NULL) {
      // Switching to an audio stream in primary mux requires a full
      // resync
      HLS_TRACE(h, "Force primary stream to resynchronize due to subtitle switch");
      h->h_primary.hd_req = h->h_primary.hd_current;
    } else {

      media_pipe_t *mp = h->h_mp;
      mp->mp_subtitle.mq_demuxer_flags &= ~HLS_QUEUE_MERGE;
      mp->mp_subtitle.mq_demuxer_flags |= HLS_QUEUE_KEYFRAME_SEEN;
      HLS_TRACE(h, "Subtitle queue merge started");
    }


    hd->hd_current = hv;

    hls_free_mbp(h->h_mp, &hd->hd_mb);
  }

  return hls_demuxer_get(hd);
}

/**
 *
 */
static media_buf_t *
hls_demuxer_get_audio(hls_t *h)
{
	if(h->h_mp->mp_subtitle.mq_stream2 != -1)
	{
		hls_demuxer_t *hds = &h->h_subtitle;
		if(hds->hd_current_stream != hds->hd_pending_stream) h->h_last_decoded_sub = -1;

		if(h->h_last_enqueued_seq > h->h_last_decoded_sub + 3 || h->h_last_enqueued_seq < h->h_last_decoded_sub - 3)
		{
			h->h_last_decoded_sub = h->h_last_enqueued_seq+1;
			hls_demuxer_get_subtitle(h);
			hls_demuxer_get_subtitle(h);
			hls_demuxer_get_subtitle(h);
			//hls_demuxer_get_subtitle(h);
		}

		if(h->h_last_enqueued_seq > h->h_last_decoded_sub)
		{
			h->h_last_decoded_sub = h->h_last_enqueued_seq;
			hls_demuxer_get_subtitle(h);
			//hls_demuxer_get_subtitle(h);
		}
	}
	else
		h->h_last_decoded_sub = -1;


  hls_demuxer_t *hd = &h->h_audio;

  if(hd->hd_current_stream != hd->hd_pending_stream) {

    hd->hd_current_stream = hd->hd_pending_stream;

    hls_variant_t *hv;
    TAILQ_FOREACH(hv, &hd->hd_variants, hv_link)
      if(hv->hv_audio_stream == hd->hd_current_stream)
        break;

    HLS_TRACE(h, "Audio demuxer checking stream switch to streamid: %d -> %s",
              hd->hd_current_stream,
              hv ? hv->hv_name : "<muxed in primary>");

    if(hd->hd_current != NULL) {
      hls_variant_close(hd->hd_current);
    }


    if(hd->hd_current != NULL && hv == NULL) {
      // Switching to an audio stream in primary mux requires a full
      // resync
      HLS_TRACE(h, "Force primary stream to resynchronize due to audio switch");
      h->h_primary.hd_req = h->h_primary.hd_current;
    } else {

      media_pipe_t *mp = h->h_mp;
      mp->mp_audio.mq_demuxer_flags |= HLS_QUEUE_MERGE;
      mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
      HLS_TRACE(h, "Audio queue merge started");
    }

    hd->hd_current = hv;

    hls_free_mbp(h->h_mp, &hd->hd_mb);
  }

  return hls_demuxer_get(hd);
}


/**
 *
 */
static void
hls_demuxer_seek(media_pipe_t *mp, hls_demuxer_t *hd, int64_t pos)
{
  hd->hd_seek_to_segment = pos;

  if(hd->hd_current != NULL && hd->hd_current->hv_demuxer_flush)
    hd->hd_current->hv_demuxer_flush(hd->hd_current);

  hls_free_mbp(mp, &hd->hd_mb);
}


/**
 *
 */
#if 0
static void __attribute__((unused))
print_ts(media_buf_t *mb)
{
  if(mb == HLS_EOF)
    printf("%5s:%8s:%5s:%-20s %-20s", "", "", "", "EOF", "");
  else if(mb == HLS_DIS)
    printf("%5s:%8s:%5s:%-20s %-20s", "", "", "", "DIS", "");
  else if(mb->mb_dts == PTS_UNSET)
    printf("%5s:%08x:%5d:%-20s %-20s",
           mb->mb_data_type == MB_VIDEO ? "VIDEO" : "AUDIO",
           mb->mb_epoch, mb->mb_sequence, "UNSET", "");
  else
    printf("%5s:%08x:%5d:%-20"PRId64" %-20"PRId64,
           mb->mb_data_type == MB_VIDEO ? "VIDEO" : "AUDIO",
           mb->mb_epoch, mb->mb_sequence, mb->mb_dts, mb->mb_user_time);
}
#endif
/**
 *
 */
static hls_demuxer_t *
get_media_buf(hls_t *h)//, int audio_first, int force)
{
  media_buf_t *b1, *b2;//, *b3;
  hls_demuxer_t *hd;
  media_pipe_t *mp = h->h_mp;

  //const int dumpinfo = 0;
 again:
  //if((b3 = hls_demuxer_get_subtitle(h)) == NULL)
  //  return NULL;

  if((b1 = hls_demuxer_get(&h->h_primary)) == NULL)
    return NULL;

  if((b2 = hls_demuxer_get_audio(h)) == NULL)
    return NULL;

#if 0
if(!audio_first)
{
  if((b1 = hls_demuxer_get(&h->h_primary)) == NULL)
    return NULL;

  //if((b2 = hls_demuxer_get_audio(h)) == NULL)
  //  return NULL;

	/*if(!force && b1 != HLS_DIS && b1 != HLS_EOF)
	{
		hd = &h->h_primary;
		return hd;
	}
	*/

	if(force)
	{
		if((b2 = hls_demuxer_get_audio(h)) == NULL)
			return NULL;
	} else return NULL;
}
else
{
  if((b2 = hls_demuxer_get_audio(h)) == NULL)
    return NULL;

  //if((b1 = hls_demuxer_get(&h->h_primary)) == NULL)
  //  return NULL;

	/*if(!force && b2 != HLS_DIS && b2 != HLS_EOF)
	{
		hd = &h->h_audio;
		return hd;
	}
	*/

	if(force)
	{
		if((b1 = hls_demuxer_get(&h->h_primary)) == NULL)
			return NULL;
	} else return NULL;
}
#endif
/*
  if(dumpinfo) {
    printf("MUXER: ");
    print_ts(b1);
    printf("   ");
    print_ts(b2);
    printf("   ");
  }
  const char *why;
*/

  if(b1 == HLS_EOF && b2 == HLS_EOF) {
    // All demuxers are EOF
    mp->mp_eof = 1;
    return HLS_EOF;
  }

  mp->mp_eof = 0;

  if(b1 == HLS_DIS) {
    if(b2 == HLS_EOF) {
      h->h_primary.hd_discontinuity = 0;
      h->h_primary.hd_mb = NULL;
      //if(dumpinfo) printf("Primary discontinuity reset\n");
	  //audio_first = 0;
      goto again;
    }

    if(b2 == HLS_DIS) {
      h->h_primary.hd_discontinuity = 0;
      h->h_audio.hd_discontinuity = 0;
      h->h_primary.hd_mb = NULL;
      h->h_audio.hd_mb = NULL;
      //if(dumpinfo) printf("Both discontinuity reset\n");
	  //audio_first = 0;
      goto again;
    }
    hd = &h->h_audio;
    //why = "primary-discontinuity";
  } else if(b2 == HLS_DIS) {

    if(b1 == HLS_EOF) {
      h->h_audio.hd_discontinuity = 0;
      h->h_audio.hd_mb = NULL;
      //if(dumpinfo) printf("Audio discontinuity reset\n");
	  //audio_first = 1;
      goto again;
    }

    hd = &h->h_primary;
    //why = "audio-discontinuity";
  } else if(b2 == HLS_EOF) {
    hd = &h->h_primary;
    //why = "audio-eof";
  } else if(b1 == HLS_EOF) {
    hd = &h->h_audio;
    //why = "primary-eof";
  } else if(b1 != NULL && b1->mb_dts == PTS_UNSET) {
    hd = &h->h_primary;
    //why = "primary-no-dts";
  } else if(b2 != NULL && b2->mb_dts == PTS_UNSET) {
    hd = &h->h_audio;
    //why = "audio-no-dts";
  }/* else if(b3 != NULL && b3->mb_dts == PTS_UNSET) {
    hd = &h->h_subtitle;
    why = "subtitle-no-dts";
  }*/ else if(b1 != NULL && b2 != NULL && b1->mb_dts < b2->mb_dts) {
    hd = &h->h_primary;
    //why = "primary-lower-dts";
  } else {
    hd = &h->h_audio;
    //why = "audio-lower-dts";
  }

  if(b1 == NULL && hd == &h->h_primary)
	  return NULL;

  if(b2 == NULL && hd == &h->h_audio)
	  return NULL;

  //if(dumpinfo) { printf("%s\n", why); }

  return hd;
}

/**
 *
 */
static void
clear_ts_offsets(hls_demuxer_t *hd)
{
  hls_variant_t *hv;
  hd->hd_last_dts = PTS_UNSET;
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link) {
    hls_segment_t *hs;
    TAILQ_FOREACH(hs, &hv->hv_segments, hs_link) {
      hs->hs_ts_offset = PTS_UNSET;
    }
  }
}


/**
 *
 */
static void
clear_hds_offset(hls_t *h)
{
  hls_discontinuity_segment_t *hds;

  LIST_FOREACH(hds, &h->h_discontinuity_segments, hds_link)
    if(hds->hds_seq != 0)
      hds->hds_offset = PTS_UNSET;
}

/**
 *
 */
static void
hls_seek(hls_t *h, int64_t ts)
{
  media_pipe_t *mp = h->h_mp;

  //mp_flush(mp);

  if(((mp->mp_hls_source & 3)==2) || ((mp->mp_hls_source & 3)==3)) // fmp4 or unmuxed aac
  {
	if(!h->h_audio.hd_current)
	{
		//TRACE(TRACE_ERROR, "fMP4", "V1 Seeking video to %"PRId64, ts);
		ts = hv_find_segment_time_offset(h->h_primary.hd_current, ts, 0); // seek to start of segment for ts position
		//mp->mp_seek_base = ts;
		//TRACE(TRACE_ERROR, "fMP4", "V2 Seeking video to %"PRId64, ts);
	}
	else
	{
		//TRACE(TRACE_ERROR, "fMP4", "A1 Seeking audio to %"PRId64, ts);
		ts = hv_find_segment_time_offset(h->h_audio.hd_current, ts, 0); // seek to start of segment for ts position
		//mp->mp_seek_base = ts;
		//TRACE(TRACE_ERROR, "fMP4", "A2 Seeking audio to %"PRId64, ts);
	}

	//TRACE(TRACE_ERROR, "fMP4", "Seeking to %"PRId64, ts);
  }

  //mp_flush(mp);
  HLS_TRACE(h, "Seeking to %"PRId64, ts);

  /*mp->mp_video.mq_seektarget = ts;
  mp->mp_audio.mq_seektarget = ts;
  mp->mp_subtitle.mq_seektarget = ts;
  */

  hls_demuxer_seek(mp, &h->h_primary, ts);
  hls_demuxer_seek(mp, &h->h_audio,   ts);
  hls_demuxer_seek(mp, &h->h_subtitle,ts);
  clear_hds_offset(h);
  clear_ts_offsets(&h->h_primary);
  clear_ts_offsets(&h->h_audio);
  clear_ts_offsets(&h->h_subtitle);
  cancellable_reset(h->h_primary.hd_cancellable);
  cancellable_reset(h->h_audio.hd_cancellable);
  cancellable_reset(h->h_subtitle.hd_cancellable);

  mp->mp_seek_base = ts;
  mp->mp_video.mq_seektarget = ts;
  mp->mp_audio.mq_seektarget = ts;
  mp->mp_subtitle.mq_seektarget = ts;
  mp_flush(mp);

  mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
  mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
}

/**
 *
 */
static event_t *
hls_play(hls_t *h, media_pipe_t *mp, char *errbuf, size_t errlen,
         const video_args_t *va)
{
  event_t *e = NULL;
  const char *canonical_url = va->canonical_url;
  sub_scanner_t *ss = NULL;
  int loading = 1;
  h->h_last_decoded_sub = -1;

#if defined(__ANDROID__)
  if(strstr(APPABISUF, "x86")) gconf.video_rgb = 1;
#endif
  h->h_restartpos_last = -1;
  h->h_last_timestamp_presented = AV_NOPTS_VALUE;
  h->h_sub_scanning_done = 0;
  h->h_enqueued_something = 0;

  h->h_playback_priority = va->priority;

  mp->mp_video.mq_stream = 0;
  //mp->mp_audio.mq_stream = 1;
  //mp->mp_subtitle.mq_stream = 1;
  //mp->mp_subtitle.mq_stream2 = 1;

  if(!(va->flags & BACKEND_VIDEO_NO_AUDIO))
    mp_become_primary(mp);

  mp_hold(mp, MP_HOLD_SYNC, NULL);

  mp_configure(mp, MP_CAN_PAUSE, MP_BUFFER_DEEP, 0, "video");

  //mp->mp_pre_buffer_delay = video_settings.video_prebuffer_size * 1000000;//10000000;

  mp->mp_video.mq_seektarget = AV_NOPTS_VALUE;
  mp->mp_audio.mq_seektarget = AV_NOPTS_VALUE;
  mp->mp_subtitle.mq_seektarget = AV_NOPTS_VALUE;
  mp->mp_seek_base = 0;

  mp->mp_pre_buffer_delay = MAX(video_settings.video_prebuffer_size, 5) * 1000000;
  mp->mp_hold_flags |= MP_HOLD_PRE_BUFFERING;


  mp_event_set_callback(mp, hls_event_callback, h);

#if defined(__ANDROID__)
  //gconf.f_in_video_paused = 0;
#endif
  int start_resume = 0;
  int64_t start = playinfo_get_restartpos(canonical_url,
                                          va->title, va->resume_mode) * 1000;
  if(start < 6000000) start = 0;
  else
  //if(start)
  {
    //TRACE(TRACE_DEBUG, "HLS", "Attempting to resume from %.2f seconds", start / 1000000.0f);

	start_resume = 1;
    //mp->mp_seek_base = start;
    //hls_seek(h, start);
    //mp->mp_seek_base = 0;
    //hls_seek(h, 0);
  }
  if(last_pos != -1)
  {
	  h->h_last_timestamp_presented = last_pos;
	  start = last_pos;
	  start_resume = 1;
	  last_pos = -1;
  }


  h->h_primary.hd_current = hls_select_default_variant(&h->h_primary);
  if(h->h_primary.hd_current == NULL) {
    snprintf(errbuf, errlen, "HLS playlist contains no playable variants");
    return NULL;
  }
  char video_initial[32];
  snprintf(video_initial, sizeof(video_initial), "hlsv:%i",
           h->h_primary.hd_current->hv_bitrate);
  prop_set_string(mp->mp_prop_video_track_current, video_initial);

#if defined(__ANDROID__)
  int wd = h->h_primary.hd_current->hv_width; int hg = h->h_primary.hd_current->hv_height;
  //TRACE(TRACE_ERROR, "HLS", "maxw=%i maxh=%i", wd, hg);
  hls_get_max_width_height(&h->h_primary, &wd, &hg);
  if((!wd || !hg) && !h->h_primary.hd_no_functional_streams)
  {
		hls_demuxer_get(&h->h_primary);
		wd = h->h_primary.hd_current->hv_width;
		hg = h->h_primary.hd_current->hv_height;
		//TRACE(TRACE_INFO, "HLS", "maxw=%i maxh=%i", wd, hg);
  }
  mp->mp_hls_source_max_width = (wd?wd:1920);
  mp->mp_hls_source_max_height = (hg?hg:1080);
  //TRACE(TRACE_INFO, "VD", "mp->mp_hls_source_max_width=%i mp->mp_hls_source_max_height=%i ", mp->mp_hls_source_max_width, mp->mp_hls_source_max_height);

#endif
  //h->h_audio.hd_current = hls_select_default_variant(&h->h_audio);
  //h->h_subtitle.hd_current = hls_select_default_variant(&h->h_subtitle);

  h->h_audio.hd_pending_stream = mp->mp_audio.mq_stream>0?mp->mp_audio.mq_stream:1;
  h->h_subtitle.hd_pending_stream = mp->mp_subtitle.mq_stream>0?mp->mp_subtitle.mq_stream:1;
  //TRACE(TRACE_ERROR, "HLS", "mp->mp_audio.mq_stream=%i", mp->mp_audio.mq_stream);
  //TRACE(TRACE_ERROR, "HLS", "mp->mp_subtitle.mq_stream=%i",mp->mp_subtitle.mq_stream);
  if((mp->mp_hls_source & 3)==2)
	mp->mp_subtitle.mq_stream2 = -1;

  if(va->flags & BACKEND_VIDEO_NO_SUBTITLE_SCAN)
    h->h_sub_scanning_done = 1;

#define USE_SEEN_AVPKT 0

#if USE_SEEN_AVPKT
	int64_t seen_apkt = 0;
	int64_t seen_vpkt = 0;
#endif

//int m_type = 0; //  1-audio_first

	int64_t seek_pending_ts = 0;
	int64_t seek_pending_req = 0;
#if !defined(__ANDROID__)
	gconf.f_in_video_playback = 1;
#endif

	event_dispatch(event_create_action(ACTION_PLAY));

  while(1) {

    if(h->h_primary.hd_no_functional_streams ||
       h->h_audio.hd_no_functional_streams) {
      if(h->h_last_error)
        snprintf(errbuf, errlen, "No playable streams -- %s",
                 hlserrstr[h->h_last_error]);
      else
        snprintf(errbuf, errlen, "No playable streams");
      e = NULL;
      break;
    }

#if defined(__ANDROID__)
	if(e == NULL && mp->mp_request_sw_decoder == 1)
	{
		e = event_create_type(EVENT_EOF);
		break;
	}
#endif


	if(seek_pending_req)
	{
#if defined(__ANDROID__)
		mp->mp_video_clock = PTS_UNSET;
		mp->mp_seeking = 2;
#endif
		if(arch_get_avtime() - seek_pending_req > 750000)
		{
			mp_flush(mp);
			//TRACE(TRACE_DEBUG, "Seek", "Seek request SEEKING: %lli", seek_pending_ts/1000);
			mp->mp_seek_base = seek_pending_ts;
			hls_seek(h, seek_pending_ts);
			seek_pending_req = 0;
#if defined(__ANDROID__)
			mp->mp_seeking = 0;
#endif
			if(video_settings.video_prebuffer_size)
			{
				mp->mp_pre_buffer_delay = ( (video_settings.video_prebuffer_size>1) ? video_settings.video_prebuffer_size : (video_settings.video_prebuffer_size + 4)) * 1000000;
				mp->mp_hold_flags |= MP_HOLD_PRE_BUFFERING;
				mp_hold(mp, mp->mp_hold_flags, NULL);
				//TRACE(TRACE_INFO, "Seek", "mp->mp_pre_buffer_delay = %i", mp->mp_pre_buffer_delay);
			}
			//if(seek_pending_ts < last_timestamp_presented) mp_underrun(mp);
		}
		//else { usleep(5000); continue; }
		//else TRACE(TRACE_INFO, "Seek", "Seeking too fast... (%lli)", arch_get_avtime() - seek_pending_req);
	}


    if(e == NULL)
      e = mp_dequeue_event_deadline(mp, 0);

    if(e != NULL) {
      if(event_is_action(e, ACTION_SKIP_FORWARD) ||
         event_is_action(e, ACTION_SKIP_BACKWARD) ||
#if defined(__ANDROID__)
		 event_is_action(e, ACTION_RELOAD_REPLAY) ||
#endif
		 event_is_action(e, ACTION_RELOAD_DATA) ||
         event_is_type(e, EVENT_EXIT) ||
         event_is_type(e, EVENT_PLAY_URL)) {
			if(event_is_action(e, ACTION_SKIP_FORWARD) ||
				event_is_action(e, ACTION_SKIP_BACKWARD)) { last_bw = -1; last_pos = -1; }
        break;
      }

	  //if(last_bw > 0)	hls_bw_switch(&h->h_primary, last_bw);

      if(event_is_type(e, EVENT_SEEK))
	  {
        event_ts_t *ets = (event_ts_t *)e;

		if(h->h_primary.hd_current->hv_frozen && mp->mp_duration > 10000000 && ets->ts >= mp->mp_duration)
		{
			h->h_last_timestamp_presented = mp->mp_duration;
			event_release(e);
			e = event_create_type(EVENT_EOF);
			break;
		}

		mp_hold(mp, MP_HOLD_SYNC, NULL);
#if defined(__ANDROID__)
		mp->mp_seeking = 1;
#endif

		if(!h->h_primary.hd_current->hv_frozen && (ets->ts >= mp->mp_duration)) ets->ts = MAX(0, mp->mp_duration - hv_find_segment_time_offset(h->h_primary.hd_current, ets->ts, 1) * 3);

		if(0)//h->h_primary.hd_current->hv_frozen && (mp->mp_hls_source&2 || (mp->mp_hls_source&3)==3)) // fmp4 or unmuxed aac
		{
			if(!h->h_audio.hd_current)
			{
				//TRACE(TRACE_ERROR, "fMP4", "V1 Seeking video to %"PRId64, ts);
				ets->ts = hv_find_segment_time_offset(h->h_primary.hd_current, ets->ts, 0); // seek to start of segment for ts position
				//TRACE(TRACE_ERROR, "fMP4", "V2 Seeking video to %"PRId64, ts);
			}
			else
			{
				//TRACE(TRACE_ERROR, "fMP4", "A1 Seeking audio to %"PRId64, ts);
				ets->ts = hv_find_segment_time_offset(h->h_audio.hd_current, ets->ts, 0); // seek to start of segment for ts position
				//TRACE(TRACE_ERROR, "fMP4", "A2 Seeking audio to %"PRId64, ts);
			}

		//TRACE(TRACE_ERROR, "fMP4", "Seeking to %"PRId64, ts);
		}

		seek_pending_ts = ets->ts;
		seek_pending_req = arch_get_avtime();
		mp_flush(mp);
        //hls_seek(h, ets->ts);
		mp->mp_hold_flags |= MP_HOLD_PRE_BUFFERING;
		prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 1);
        loading = (MP_HOLD_PRE_BUFFERING);
		//TRACE(TRACE_DEBUG, "SEEK", "Seek initiated");
		//TRACE(TRACE_DEBUG, "SEEK", "hv_find_segment_time_offset(h->h_primary.hd_current, ets->ts, 1)=%i", (int)hv_find_segment_time_offset(h->h_primary.hd_current, ets->ts, 1));

		h->h_last_timestamp_presented = ets->ts;
		start = ets->ts;
#if USE_SEEN_AVPKT
		seen_apkt = 0;
		seen_vpkt = 0;
#endif

#if defined(__ANDROID__)
		//mp->mp_seeking = 0;
#endif
		event_release(e);
		e = NULL;
		continue;
      }
      event_release(e);
      e = NULL;
    }

    if(!h->h_sub_scanning_done && h->h_duration) {
      h->h_sub_scanning_done = 1;
      ss = sub_scanner_create(h->h_baseurl, mp->mp_prop_subtitle_tracks, va,
                              h->h_duration / 1000000LL);
    }

#if 1

	/*if(!start_resume)
	{
		hls_demuxer_get_audio(h);
		hls_demuxer_get(&h->h_primary);
	}
	*/

	if(last_bw<0 && start_resume)
	{
		if(hls_demuxer_get_audio(h) != NULL)
		{
			if(h->h_audio.hd_current != NULL)
			{
				//TRACE(TRACE_ERROR, "HLS", "hv_dur_aud = %.0f", h->h_audio.hd_current->hv_duration/ 1000000.0f);
				if(start > h->h_audio.hd_current->hv_duration || start > 14400000000)
				{
					//TRACE(TRACE_DEBUG, "HLS", "Refusing to resume from %.2f seconds (Duration: %.2f seconds)", start / 1000000.0f, mp->mp_duration / 1000000.0f);
					start = h->h_audio.hd_current->hv_duration - 60000000;
					if(start < 0 || start > 14400000000)
						start = 0;
				}
				else
				{
					if(h->h_audio.hd_current->hv_duration - start < 60000000)
					{
						//TRACE(TRACE_DEBUG, "HLS", "Refusing to resume from %.2f seconds (Duration: %.2f seconds)", start / 1000000.0f, h->h_audio.hd_current->hv_duration / 1000000.0f);
						start = 0;
					}
				}
			}
		}
		if(hls_demuxer_get(&h->h_primary) != NULL)
		{
			if(h->h_primary.hd_current != NULL)
			{
				//TRACE(TRACE_ERROR, "HLS", "hv_duration = %.0f", h->h_primary.hd_current->hv_duration/ 1000000.0f);
				if(start > h->h_primary.hd_current->hv_duration || start > 14400000000)
				{
					//TRACE(TRACE_DEBUG, "HLS", "Refusing to resume from %.2f seconds (Duration: %.2f seconds)", start / 1000000.0f, h->h_primary.hd_current->hv_duration / 1000000.0f);
					start = h->h_primary.hd_current->hv_duration - 60000000;
					if(start < 0 || start > 14400000000)
						start = 0;
				}
				else
				{
					if(h->h_primary.hd_current->hv_duration - start < 60000000)
					{
						//TRACE(TRACE_DEBUG, "HLS", "Refusing to resume from %.2f seconds (Duration: %.2f seconds)", start / 1000000.0f, h->h_primary.hd_current->hv_duration / 1000000.0f);
						start = 0;
					}
				}
			}
		}

		start_resume = 0;

		if(start)
		{
			TRACE(TRACE_DEBUG, "HLS", "Resuming from %.0f seconds", start / 1000000.0f);
			mp->mp_seek_base = start;
			hls_seek(h, start);
			seek_pending_req = 0;
			if((mp->mp_hls_source & 3) == 2)
			{
				mp_enqueue_event_locked(mp, event_create_action(ACTION_SEEK_BACKWARD_FRAME));
				mp_enqueue_event_locked(mp, event_create_action(ACTION_PLAY));
			}
		}
		continue;
	}
#endif

	if(seek_pending_req) { usleep(5000); continue; }

	hls_demuxer_t *hd = NULL;

	hd = get_media_buf(h);//, m_type, 1);
	//if(hd == NULL)
	//	continue;

#if 0
    //hls_demuxer_t *
	hd = get_media_buf(h, m_type, 0);
	m_type ^= 1;
    if(hd == NULL)
	{
		//bw_start = arch_get_avtime();
		hd = get_media_buf(h, m_type, 0);
		m_type ^= 1;

		if(hd == NULL)
		{
			//bw_start = arch_get_avtime();
			hd = get_media_buf(h, m_type, 1);
			m_type ^= 1;

			if(hd == NULL)
			{
				//bw_start = arch_get_avtime();
				hd = get_media_buf(h, m_type, 1);
				m_type ^= 1;
			}
		}

		if(hd == NULL)
			continue;
	}
#endif


  if(unlikely(mp->mp_hold_flags & MP_HOLD_PRE_BUFFERING)) {
		if(mp->mp_buffer_delay > mp->mp_pre_buffer_delay) {
		  mp->mp_hold_flags &= ~MP_HOLD_PRE_BUFFERING;
		  mp_set_playstatus_by_hold_locked(mp, NULL);
		}
	}

    if(hd == HLS_EOF)
	{
#if 0
		int is_empty = 0;
		hts_mutex_lock(&mp->mp_mutex);

		is_empty =
			TAILQ_FIRST(&mp->mp_audio.mq_q_data) == NULL;
			/*
		is_empty =
			TAILQ_FIRST(&mp->mp_audio.mq_q_data) == NULL &&
			TAILQ_FIRST(&mp->mp_video.mq_q_data) == NULL &&
			TAILQ_FIRST(&mp->mp_subtitle.mq_q_data) == NULL;
		*/
      hts_mutex_unlock(&mp->mp_mutex);

      if(!is_empty) {
        usleep(300000);
		hts_mutex_lock(&mp->mp_mutex);
		is_empty =
			TAILQ_FIRST(&mp->mp_audio.mq_q_data) == NULL &&
			TAILQ_FIRST(&mp->mp_video.mq_q_data) == NULL;
		hts_mutex_unlock(&mp->mp_mutex);
      }

      if(!is_empty) {
        usleep(10000);
        continue;
      }
#endif
		e = mp_wait_for_empty_queues(mp);

		if(e == NULL)
		{
#if defined(__ANDROID__)
				if(mp->mp_audio_created!=3)
				{
					for(int u=0;u<4;u++)
					{
						//TRACE(TRACE_DEBUG, "AT-PL","mp->mp_audio_drained=%i", mp->mp_audio_drained);
						if(TAILQ_FIRST(&mp->mp_audio.mq_q_data) == NULL && !mp->mp_audio_drained)
						{
							usleep(250000);
						} else if(mp->mp_audio_drained) break;
					}
				}
#endif
			e = event_create_type(EVENT_EOF);
			break;
		}
		else
		{
			usleep(10000);
			continue;
		}

      //e = event_create_type(EVENT_EOF);
      //break;

    }
	else if(hd != NULL)
	{

		media_buf_t *mb = hd->hd_mb;
		media_queue_t *mq;
		mq = &mp->mp_video;

		//HLS_TRACE(h, "%s media queue MQ PACKET",
		//     mb->mb_data_type == MB_VIDEO ? "VIDEO" : (mb->mb_data_type == MB_AUDIO ? "AUDIO" : "SUBTITLE"));

		//if(mb->mb_data_type == MB_VIDEO || mb->mb_data_type == MB_SUBTITLE)
		//  mq = &mp->mp_video;
		if(mb->mb_data_type == MB_AUDIO)
			mq = &mp->mp_audio;
#if !defined(NACL)
#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
		//else
		{
			if(mp->mp_buffer_delay>30000000
#if defined(__ANDROID__)
					&& !gconf.disable_cpu_optimize /*&& !mp->mp_tunnel_mode*/
#endif
			)
			{
				//TRACE(TRACE_DEBUG, "slp", "dly=%i slp=%i", mp->mp_buffer_delay/1000, ((mp->mp_buffer_delay>>14)&0x1fff)|0x7ff );
				//usleep( ((mp->mp_buffer_delay>>14)&0x1fff)|0x7ff );

				if(mp->mp_buffer_delay>60000000)
					usleep(4096);
				else
					usleep(1536);

				//usleep(1000);
			}
		}
#endif
#endif

#if USE_SEEN_AVPKT
		static int frames_v = 0;
		static int frames_a = 0;

		if(seen_apkt != 1 && ( (mp->mp_hls_source & 3)== 1) )
		{
			if(mb->mb_data_type == MB_VIDEO) frames_v++;
			if(mb->mb_data_type == MB_AUDIO) frames_a++;
			//TRACE(TRACE_ERROR, "HLS", "DELTA PTS: %i  FRAMES: %i", (int)(seen_apkt-seen_vpkt), frames);

			if(!seen_vpkt && mb->mb_data_type == MB_VIDEO)
			{
				seen_vpkt = 2 + mb->mb_pts;
				//TRACE(TRACE_ERROR, "Video", "VIDEO PTS: %ld", seen_vpkt);
			}
			else
			if(!seen_apkt && mb->mb_data_type == MB_AUDIO)
			{
				seen_apkt = 2 + mb->mb_pts;
				//TRACE(TRACE_ERROR, "Video", "AUDIO PTS: %ld", seen_apkt);
			}

			if(frames_v>15 && frames_a>120) // only for muxed in audio (no fMP4/avc+aac)
			{
				if(seen_apkt && seen_vpkt && seen_apkt!=1)
				{
					if( ((seen_apkt-seen_vpkt)>-1000000 && (seen_apkt-seen_vpkt)<1000000) || frames_v > 59  || frames_a > 119 )
					{
						//TRACE(TRACE_INFO, "HLS", "SYNC DELTA PTS: %i FRAME V/A: %i/%i TS: %i", (int)(seen_apkt-seen_vpkt), frames_v, frames_a, (int)(h->h_last_timestamp_presented/1000));
						//TRACE(TRACE_DEBUG, "HLS", "Resuming after A/V sync (%i ms) from position %.0f sec", (int)(seen_apkt-seen_vpkt)/1000, start / 1000000.0f);
						frames_v = 0;
						frames_a = 0;
						seen_apkt = 1;
						mp->mp_seek_base = h->h_last_timestamp_presented;//MAX(0, h->h_last_timestamp_presented-50000);
						hls_seek(h, mp->mp_seek_base);
						continue;
					}
				}
			}
		}
#endif

#if 0 //defined(__ANDROID__)
		f_bytes_downloaded += mb->mb_size;
		time_t now = time(NULL);
		if(now!=f_download_rate.last)
		{
			average_fill(&f_download_rate, now, f_bytes_downloaded);
			prop_set_float(mp->mp_video.mq_prop_bw, average_read(&f_download_rate, now) / 125000.f);
		}
#endif

		e = enqueue_buffer(mp, mq, mb, h, hd->hd_current);

		if(e == NULL)
			hd->hd_mb = NULL;
    }

	if(loading != (mp->mp_hold_flags & MP_HOLD_PRE_BUFFERING))
	{
		loading = (mp->mp_hold_flags & MP_HOLD_PRE_BUFFERING);
		prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, (loading!=0));
    }

#if 1
      if(start_resume)
	  {
		if(mp->mp_duration) //  && (mp->mp_flags & MP_CAN_SEEK)
		{
			if(start > mp->mp_duration || start > 14400000000)
				TRACE(TRACE_DEBUG, "HLS", "Refusing to resume from %.2f seconds (Duration: %.2f seconds)", start / 1000000.0f, mp->mp_duration / 1000000.0f);
			else
			{
				if(mp->mp_duration - start < 60000000) start = 0;
				TRACE(TRACE_DEBUG, "HLS", "Resuming from %.2f seconds (post-init)", start / 1000000.0f);
				mp->mp_seek_base = start;
				hls_seek(h, start);
				mp_hold(mp, MP_HOLD_SYNC, NULL);
				mp->mp_hold_flags |= MP_HOLD_PRE_BUFFERING;
			}
			start_resume = 0;
		}
      }
#endif
  }

#if defined(__ANDROID__)
	//gconf.f_in_video_paused = 0;
	// !h->h_primary.hd_current->hv_frozen
	if(event_is_action(e, ACTION_RELOAD_REPLAY))
	{
		event_release(e);
		//if((mp->mp_flags & MP_CAN_SEEK) && (h->h_last_timestamp_presented != PTS_UNSET))
		//	TRACE(TRACE_INFO, "HLS", "Trying to restart playback from %.2f sec", h->h_last_timestamp_presented / 1000000.0f);
		e = event_create_type(EVENT_REOPEN);
		//e = event_create_action(ACTION_SKIP_SAME);
		//e = event_create_playurl(.url = "bgTV:channel:8d2fa156247b23ca6ae07ee88b60e446", .primary = 1);
	}
#endif
  if((mp->mp_flags & MP_CAN_SEEK) && (h && h->h_primary.hd_current && h->h_primary.hd_current->hv_frozen)) {

    // Compute stop position (in percentage of video length)

    int spp = mp->mp_duration ? mp->mp_seek_base * 100 / mp->mp_duration : 0;
	if(spp>98) spp = 100;

    if(spp >= video_settings.played_threshold || event_is_type(e, EVENT_EOF)) {
      playinfo_set_restartpos(canonical_url, -1, 0);
	  if(h->h_primary.hd_current->hv_frozen)
	  {
		playinfo_register_play(canonical_url, 1);
		TRACE(TRACE_DEBUG, "Video",
			"Playback reached %d%%%s, counting as played", // (%s)
			spp, event_is_type(e, EVENT_EOF) ? ", EOF detected" : "" /*, canonical_url*/);
	  }
    } else if(h->h_last_timestamp_presented != PTS_UNSET)
	{
		//TRACE(TRACE_DEBUG, "Video", "TS: %li, DUR: %li", h->h_last_timestamp_presented/1000, mp->mp_duration/1000);
		if(mp->mp_duration >0 && h->h_last_timestamp_presented > mp->mp_duration)
			playinfo_set_restartpos(canonical_url, -1, 0);
		else
		{
			if(h->h_primary.hd_current->hv_frozen)
			{
				int64_t rrp = hv_find_segment_time_offset(h->h_primary.hd_current, h->h_last_timestamp_presented, 0);
				//rrp -= hv_find_segment_time_offset(h->h_primary.hd_current, h->h_last_timestamp_presented, 1);
				//rrp = MAX(0, rrp);
				//TRACE(TRACE_DEBUG, "Video", "FROZEN TS: %lli, RRP: %lli [%s]", h->h_last_timestamp_presented, rrp, canonical_url);
				playinfo_set_restartpos(canonical_url, rrp / 1000, 0);
			}
			else
			{
				playinfo_set_restartpos(canonical_url, -1, 0);
				//TRACE(TRACE_DEBUG, "Video", "LIVE TS: %lli", h->h_last_timestamp_presented);
			}
		}
    }
  }
  // Shutdown

  // event_dispatch(event_create_action(ACTION_PAUSE));
	usleep(500000);

  mp_event_set_callback(mp, NULL, NULL);

	mp->mp_seek_base = 0;
	mp->mp_video.mq_seektarget = AV_NOPTS_VALUE;
	mp->mp_audio.mq_seektarget = AV_NOPTS_VALUE;

  mp_shutdown(mp);

  sub_scanner_destroy(ss);

  //last_bw = -1;
  gconf.f_in_video_playback = 0;
  return e;
}


/**
 *
 */
int
hls_get_audio_track(hls_t *h, int pid, const char *mux_id, const char *language,
                    const char *fmt, const char *chans, int autosel)
{
	hls_audio_track_t *hat;
	char trackuri[256];
	char title[64];
	snprintf(title, sizeof(title), "Audio");

	rstr_t *rformat     = rstr_alloc(fmt);
	rstr_t *rlanguage   = rstr_alloc(language);

	if(!fmt) sprintf(title, "%s%s", "Audio", (chans&&strstr(chans, "6")?" 5.1":""));
	else if(strstr(fmt, "EAC4") || strstr(fmt, "eac4") || strstr(fmt, "ac-4") || strstr(fmt, "Atmos") || strstr(fmt, "ec-3;spatial") || strstr(fmt, "mp4a.a6;spatial")) sprintf(title, "Dolby Atmos%s", (chans&&strstr(chans, "6")?" 5.1":""));
	else if(strstr(fmt, "EAC3") || strstr(fmt, "eac3") || strstr(fmt, "ec-3") || strstr(fmt, "ec+3") || strstr(fmt, "eac-3") || strstr(fmt, "dec3") || strstr(fmt, "mp4a.a6")) sprintf(title, "Dolby Digital+%s", (chans&&strstr(chans, "6")?" 5.1":""));
	else if(strstr(fmt, "AC3") || strstr(fmt, "ac3") || strstr(fmt, "ac-3") || strstr(fmt, "dac3") || strstr(fmt, "mp4a.a5")) sprintf(title, "Dolby Digital%s", (chans&&chans&&strstr(chans, "6")?" 5.1":""));
	else if(strstr(fmt, "DTS") || strstr(fmt, "dts") || strstr(fmt, "mp4a.a9") || strstr(fmt, "mp4a.aa")) sprintf(title, "DTS%s", (chans&&chans&&strstr(chans, "6")?" 5.1":""));
	else if(strstr(fmt, "AAC") || strstr(fmt, "MP3") || strstr(fmt, "mp4a") || strstr(fmt, "mp3")) sprintf(title, "%s", (chans&&strstr(chans, "6")?"Multichannel 5.1":"Stereo"));

	rstr_t *rtitle      = rstr_alloc(title);

  LIST_FOREACH(hat, &h->h_audio_tracks, hat_link) {
    if(mux_id == NULL && hat->hat_mux_id == NULL && hat->hat_pid == pid)
      break;
    if(mux_id != NULL && hat->hat_mux_id != NULL &&
       !strcmp(hat->hat_mux_id, mux_id))
      break;
  }

  if(hat != NULL) {

    prop_set(hat->hat_trackprop, "format", PROP_SET_RSTRING, rformat);
	if(chans != NULL)
		prop_set(hat->hat_trackprop, "title", PROP_SET_RSTRING, rtitle);
  } else {

    hat = LIST_FIRST(&h->h_audio_tracks);
    int newid = hat ? hat->hat_stream_id + 1 : 1;

    hat = calloc(1, sizeof(hls_audio_track_t));
    hat->hat_stream_id = newid;
    hat->hat_pid = pid;
    hat->hat_mux_id = mux_id ? strdup(mux_id) : NULL;
    LIST_INSERT_HEAD(&h->h_audio_tracks, hat, hat_link);
    snprintf(trackuri, sizeof(trackuri), "hls:%d", newid);

    prop_t *s;
    int score = 500000;

    if(mux_id) {
      s = _p("Supplementary");
    } else {
      s = _p("Primary");
      score += 10;
    }

    hat->hat_trackprop =
      mp_add_trackr(h->h_mp->mp_prop_audio_tracks,
                    rtitle,
                    trackuri,
                    rformat,
                    rformat,
                    rlanguage,
                    NULL,
                    s,
                    score,
                    autosel);
  }
  rstr_release(rformat);
  rstr_release(rlanguage);
  rstr_release(rtitle);

  return hat->hat_stream_id;
}

static void
hls_get_video_track(hls_t *h, hls_variant_t *hv)
{
	if(hv->hv_codec==AV_CODEC_ID_HEVC && video_settings.hls_limit >= 6 && video_settings.hls_limit <= 11) return;
	if(hv->hv_codec==AV_CODEC_ID_AV1  && video_settings.hls_limit >= 6) return;

	char trackuri[32];
	char title[64];
	char fmt[8];
	//char hls[4];
	char fps[32];
	int sort_w = 0;

	snprintf(trackuri, sizeof(trackuri), "hlsv:%i", hv->hv_bitrate);
	if(!hv->hv_width || !hv->hv_height)
		snprintf(title, sizeof(title), "No Video, %i kb/s", hv->hv_bitrate/1000);
	else
	{
		if(hv->hv_bitrate>4500000)
			snprintf(title, sizeof(title), "%ix%i, %i Mb/s", hv->hv_width, hv->hv_height, (hv->hv_bitrate+500000)/1000000);
		else
			snprintf(title, sizeof(title), "%ix%i, %i kb/s", hv->hv_width, hv->hv_height, hv->hv_bitrate/1000);
	}
	snprintf(fmt, sizeof(fmt), "MPEG-TS");

	//snprintf(hls, sizeof(hls), (last_bw==hv->hv_bitrate?"HLC":"HLS"));
	if(hv->hv_fps>0) snprintf(fps, sizeof(fps), "%.6g fps %s", hv->hv_fps, (hv->hv_codec_hdr?"HDR":"")); else fps[0]=0;

	if(hv->hv_codec==AV_CODEC_ID_AV1) { snprintf(fmt, sizeof(fmt), "%sAV1", (hv->hv_codec_hdr==2?"DV-":"")); sort_w = 70; }
	if(hv->hv_codec==AV_CODEC_ID_HEVC)  {snprintf(fmt, sizeof(fmt), "%sHEVC", (hv->hv_codec_hdr==2?"DV-":"")); sort_w = 60; }
	if(hv->hv_codec==AV_CODEC_ID_VP9) { snprintf(fmt, sizeof(fmt), "VP9"); sort_w = 50; }
	if(hv->hv_codec==AV_CODEC_ID_H264)  {snprintf(fmt, sizeof(fmt), "%sAVC", (hv->hv_codec_hdr==2?"DV-":"")); sort_w = 40; }
	if(hv->hv_codec==AV_CODEC_ID_MPEG2VIDEO) { snprintf(fmt, sizeof(fmt), "MPEG2"); sort_w = 30; }

	rstr_t *rtitle      = rstr_alloc(title);
	rstr_t *rformat     = rstr_alloc(fmt);
	//rstr_t *rHLS  	    = rstr_alloc(hls);
	rstr_t *rfps  	    = rstr_alloc(fps);

    //prop_t *s; s = _p("Adaptive");

    (void)!mp_add_trackr(h->h_mp->mp_prop_audio_tracks,
                    rtitle,
                    trackuri,
                    rformat,
                    rformat,
                    rfps,
                    NULL, //rHLS,
                    NULL,
                    (hv->hv_bitrate/10000)+sort_w*1000,
                    (last_bw==hv->hv_bitrate)?1:0);

	rstr_release(rfps);
	//rstr_release(rHLS);
	rstr_release(rformat);
	rstr_release(rtitle);

	return;
}

/**
 *
 */
static void
hls_free_audio_tracks(hls_t *h)
{
  hls_audio_track_t *hat;

  while((hat = LIST_FIRST(&h->h_audio_tracks)) != NULL) {
    LIST_REMOVE(hat, hat_link);
    prop_ref_dec(hat->hat_trackprop);
    free(hat->hat_mux_id);
    free(hat);
  }
}

int
hls_get_subtitle_track(hls_t *h, int pid, const char *mux_id, const char *language,
                    const char *fmt, int autosel)
{
  hls_subtitle_track_t *hat;
  char trackuri[256];

  rstr_t *rformat     = rstr_alloc(fmt);
  rstr_t *rlanguage   = rstr_alloc(language);

  LIST_FOREACH(hat, &h->h_subtitle_tracks, hat_link) {
    if(mux_id == NULL && hat->hat_mux_id == NULL && hat->hat_pid == pid)
      break;
    if(mux_id != NULL && hat->hat_mux_id != NULL &&
       !strcmp(hat->hat_mux_id, mux_id))
      break;
  }

  if(hat != NULL) {

    prop_set(hat->hat_trackprop, "format", PROP_SET_RSTRING, rformat);

  } else {

    hat = LIST_FIRST(&h->h_subtitle_tracks);
    int newid = hat ? hat->hat_stream_id + 1 : 1;

    hat = calloc(1, sizeof(hls_subtitle_track_t));
    hat->hat_stream_id = newid;
    hat->hat_pid = pid;
    hat->hat_mux_id = mux_id ? strdup(mux_id) : NULL;
    LIST_INSERT_HEAD(&h->h_subtitle_tracks, hat, hat_link);
    snprintf(trackuri, sizeof(trackuri), strstr(fmt, "WEBVTT")?"hls:%d":"libav:%d", newid);

    prop_t *s;
    int score = 1000;

    if(mux_id) {
      s = _p("Supplementary");
    } else {
      s = _p("Embedded");
      score += 10;
    }

    hat->hat_trackprop =
      mp_add_trackr(h->h_mp->mp_prop_subtitle_tracks,
                    NULL,
                    trackuri,
                    rformat,
                    rformat,
                    rlanguage,
                    NULL,
                    s,
                    score,
                    autosel);
  }
  rstr_release(rformat);
  rstr_release(rlanguage);

  return hat->hat_stream_id;
}

/**
 *
 */
static void
hls_free_subtitle_tracks(hls_t *h)
{
  hls_subtitle_track_t *hat;

  while((hat = LIST_FIRST(&h->h_subtitle_tracks)) != NULL) {
    LIST_REMOVE(hat, hat_link);
    prop_ref_dec(hat->hat_trackprop);
    free(hat->hat_mux_id);
    free(hat);
  }
}

/**
 *
 */
static int
variant_cmp(const hls_variant_t *a, const hls_variant_t *b)
{
  return b->hv_bitrate - a->hv_bitrate;
}


/**
 *
 */
static int
check_if_bitrate_exist(const hls_demuxer_t *hd, int bitrate, int width)
{
  const hls_variant_t *hv;
  TAILQ_FOREACH(hv, &hd->hd_variants, hv_link)
    if(hv->hv_bitrate == bitrate && hv->hv_width == width)
      return 1;
  return 0;
}


/**
 *
 */
static hls_variant_t *
hls_add_variant(const hls_t *h, const char *url, hls_variant_t *hv,
		hls_demuxer_t *hd, const char *name)
{
  if(hv == NULL)
    hv = variant_create(hd);


  if(hv->hv_bitrate && check_if_bitrate_exist(hd, hv->hv_bitrate, hv->hv_width)) {
    HLS_TRACE(h, "Skipping duplicate bitrate %d (width %d) via %s",
              hv->hv_bitrate, hv->hv_width, url);
    variant_destroy(hv);
    return NULL;
  }

  if(name != NULL)
    snprintf(hv->hv_name, sizeof(hv->hv_name), "%s", name);
  else
	if(hv->hv_width && hv->hv_height)
		snprintf(hv->hv_name, sizeof(hv->hv_name), "%ix%i (%d kb/s)",
				 hv->hv_width, hv->hv_height, hv->hv_bitrate/1000);
  else
    snprintf(hv->hv_name, sizeof(hv->hv_name), "bitrate %d",
             hv->hv_bitrate);

  hv->hv_url = url_resolve_relative_from_base(h->h_baseurl, url);
  TAILQ_INSERT_SORTED(&hd->hd_variants, hv, hv_link, variant_cmp,
                      hls_variant_t);
  return hv;
}



/**
 *
 */
static int
hls_ext_x_media(hls_t *h, const char *V, int pid)
{
  char *v = mystrdupa(V);

  const char *type = NULL;
  const char *group = NULL;
  const char *name = NULL;
  const char *lang = NULL;
  const char *uri = NULL;
  const char *charact = NULL;
  const char *chans = NULL;
  int def = 0;
  int autosel = 0;

  while(*v) {
    const char *key, *value;
    v = get_attrib(v, &key, &value);
    if(v == NULL)
      break;
    if(!strcmp(key, "TYPE"))
      type = value;
    else if(!strcmp(key, "GROUP-ID"))
      group = value;
    else if(!strcmp(key, "NAME"))
      name = value;
    else if(!strcmp(key, "CHARACTERISTICS"))
      charact = value;
    else if(!strcmp(key, "CHANNELS"))
      chans = value;
    else if(!strcmp(key, "LANGUAGE"))
      lang = value;
    else if(!strcmp(key, "AUTOSELECT"))
      autosel = !strcmp(value, "YES");
    else if(!strcmp(key, "DEFAULT"))
      def = !strcmp(value, "YES");
    else if(!strcmp(key, "URI"))
      uri = value;
  }
  if(uri == NULL || type == NULL)
    return pid;


//  HLS_TRACE(h, "Secondary stream %s "
  HLS_TRACE(h, "Secondary stream "
            "(type=%s group=%s name=%s language=%s autoselect=%s default=%s)",
            //uri,
            type,
            group ? group : "<unset>",
            name  ? name  : "<unset>",
            lang  ? lang  : "<unset>",
            autosel ? "YES" : "NO",
            def ? "YES" : "NO");

  if(!strcmp(type, "AUDIO")) {
    hls_variant_t *hv =
      hls_add_variant(h, uri, NULL, &h->h_audio, name ? name : "audio");

    if(hv != NULL) {
      hv->hv_audio_stream = hls_get_audio_track(h, pid, hv->hv_url, lang,
                                                charact, chans, autosel);
	  pid++;
    }
  }

  if(!strcmp(type, "SUBTITLES")) {
    hls_variant_t *hv =
      hls_add_variant(h, uri, NULL, &h->h_subtitle, name ? name : "subtitle");

    if(hv != NULL) {
      hv->hv_subtitle_stream = hls_get_subtitle_track(h, pid, hv->hv_url, lang,
                                                "WEBVTT", autosel);
	  //hv->hv_mpeg_ts_last = -1;
	  pid++;
    }
  }
  return pid;
}


// from https://developer.apple.com/library/ios/documentation/networkinginternet/conceptual/streamingmediaguide/StreamingMediaGuide.pdf

static const struct {
  const char *name;
  int profile;
  int level;
} AVC_h264_codecs[] = {

  // Baseline
  { "avc1.42001e",   66, 30},
  { "avc1.42c01e",   66, 30},
  { "avc1.42c028",   66, 40},
  { "avc1.66.30",    66, 30},
  { "avc1.42001f",   66, 31},

  // Main
  { "avc1.4d001e",   77, 30},
  { "avc1.77.30",    77, 30},
  { "avc1.4d001f",   77, 31},
  { "avc1.4d0028",   77, 40},
  { "avc1.4d401e",   77, 30},
  { "avc1.4d401E",   77, 30},
  { "avc1.4d401f",   77, 31},
  { "avc1.4D401F",   77, 31},
  { "avc1.4d4020",   77, 32},
  { "avc1.4D4020",   77, 32},
  { "avc1.4d402a",   77, 42},
  { "avc1.4D402A",   77, 42},

  // High
  { "avc1.640015",   100, 21},
  { "avc1.64001e",   100, 30},
  { "avc1.64001f",   100, 31},
  { "avc1.640020",   100, 32},
  { "avc1.640028",   100, 40},
  { "avc1.640029",   100, 41}, // Spec says 4.0 but that must be a typo
  { "avc1.64002a",   100, 42},

  // Main 8bit
  { "hev1.1.6.L60.B0",	177, 20},
  { "hvc1.1.6.L60.B0",	177, 20},  //720@30
  { "hev1.1.6.L63.B0",	177, 21},
  { "hvc1.1.6.L63.B0",	177, 21},  //720@30
  { "hev1.1.6.L90.B0",	177, 30},
  { "hvc1.1.6.L90.B0",	177, 30},  //720p@60 1080p@30
  { "hev1.1.6.L93.B0",	177, 31},
  { "hvc1.1.6.L93.B0",	177, 31},  //720p@60 1080p@30
  { "hev1.1.6.L120.B0",	177, 40},
  { "hvc1.1.6.L120.B0",	177, 40},  //1080p@60
  { "hev1.1.6.L123.B0",	177, 41},
  { "hvc1.1.6.L123.B0",	177, 41},  //1080@60 2160p@30
  { "hev1.1.6.L150.B0",	177, 50},
  { "hvc1.1.6.L150.B0",	177, 50},	//2160p@60
  { "hev1.1.6.L153.B0",	177, 51},
  { "hvc1.1.6.L153.B0",	177, 51},

  // Main 10bit (HDR)
  { "hev1.2.4.L60.B0",	178, 20},
  { "hvc1.2.4.L60.B0",	178, 20},  //720@30
  { "hev1.2.4.L63.B0",	178, 21},
  { "hvc1.2.4.L63.B0",	178, 21},  //720@30
  { "hev1.2.4.L90.B0",	178, 30},
  { "hvc1.2.4.L90.B0",	178, 30},  //720p@60 1080p@30
  { "hev1.2.4.L93.B0",	178, 31},
  { "hvc1.2.4.L93.B0",	178, 31},  //720p@60 1080p@30
  { "hev1.2.4.L120.B0",	178, 40},
  { "hvc1.2.4.L120.B0",	178, 40},  //1080p@60
  { "hev1.2.4.L123.B0",	178, 41},
  { "hvc1.2.4.L123.B0",	178, 41},  //1080@60 2160p@30
  { "hev1.2.4.L150.B0",	178, 50},
  { "hvc1.2.4.L150.B0",	178, 50},	//2160p@60
  { "hev1.2.4.L153.B0",	178, 51},
  { "hvc1.2.4.L153.B0",	178, 51},

};


/**
 *
 */
static void
hls_ext_x_stream_inf(hls_t *h, const char *V, hls_variant_t **hvp,
                     hls_demuxer_t *hd)
{
  char *v = mystrdupa(V);

  if(*hvp != NULL)
    free(*hvp);

  hls_variant_t *hv = *hvp = variant_create(hd);

  while(*v) {
    const char *key, *value;
    v = get_attrib(v, &key, &value);
    if(v == NULL)
      break;

    if(!strcmp(key, "BANDWIDTH"))
	{
      if(!hv->hv_bitrate) hv->hv_bitrate = atoi(value);
	}
    if(!strcmp(key, "AVERAGE-BANDWIDTH"))
      hv->hv_bitrate = atoi(value);
    else if(!strcmp(key, "AUDIO"))
      hv->hv_audio_group = strdup(value);
    else if(!strcmp(key, "SUBS"))
      hv->hv_subs_group = strdup(value);
    else if(!strcmp(key, "PROGRAM-ID"))
      hv->hv_program = atoi(value);
	else if(!strcmp(key, "DEFAULT"))
	  hv->hv_default = !strcmp(value, "YES");
    else if(!strcmp(key, "VIDEO-RANGE")) {
		if(strstr(value, "HDR") || strstr(value, "HLG") || strstr(value, "PQ")) { if(!hv->hv_codec_hdr) hv->hv_codec_hdr = 1; }
	}
    else if(!strcmp(key, "CODECS")) {
	  if(strstr(value, "dav") || strstr(value, "dva") || strstr(value, "dvh")) hv->hv_codec_hdr = 2;
      if(strstr(value, "avc1") || strstr(value, "avc3") || (strstr(value, "dvav") && !strstr(value, "dvav1")) || strstr(value, "dva1")) hv->hv_codec=AV_CODEC_ID_H264;
	  if(strstr(value, "av01") || strstr(value, "dav1") || strstr(value, "dvav1")) hv->hv_codec=AV_CODEC_ID_AV1;
	  if(strstr(value, "vp")) hv->hv_codec=AV_CODEC_ID_VP9;
	  if(strstr(value, "hev") || strstr(value, "hvc") || strstr(value, "hve") || strstr(value, "dvh")) hv->hv_codec=AV_CODEC_ID_HEVC;
	  if(strstr(value, "mpeg") || strstr(value, "mpg")) hv->hv_codec=AV_CODEC_ID_MPEG2VIDEO;
		//TRACE(TRACE_DEBUG, "HLS-CODEC", "codec_avc=%i", hv->hv_codec_avc);
      for(int i = 0; i < ARRAYSIZE(AVC_h264_codecs); i++) {
        if(strstr(value, AVC_h264_codecs[i].name)) {
          hv->hv_h264_profile = AVC_h264_codecs[i].profile;
          hv->hv_h264_level = AVC_h264_codecs[i].level;
          break;
        }
      }
    }
    else if(!strcmp(key, "RESOLUTION")) {

      const char *h = strchr(value, 'x');
      if(h != NULL) {
        hv->hv_width  = atoi(value);
        hv->hv_height = atoi(h+1);
	    //TRACE(TRACE_DEBUG, "HLS", "Video: %d x %d", hv->hv_width, hv->hv_height);
      }
    }
    else if(!strcmp(key, "FRAME-RATE")) {
     hv->hv_fps  = MAX(my_str2double(value, NULL), 23.0f);
	 if(hv->hv_fps<=23.0f || hv->hv_fps>60.f) hv->hv_fps = 0.f;
	 //else
	 //TRACE(TRACE_DEBUG, "HLS", "Variant frame-rate: %.2f fps", hv->hv_fps);
    }
  }

  //if(hv->hv_width && hv->hv_height && hv->hv_fps)
  //	  TRACE(TRACE_DEBUG, "HLS", "HLS Variant detected as %ix%i at %.3f fps", hv->hv_width, hv->hv_height, hv->hv_fps);
}


/**
 *
 */
static void
hls_demuxer_init(hls_demuxer_t *hd, hls_t *h, const char *type)
{
  TAILQ_INIT(&hd->hd_variants);
  hd->hd_hls = h;
  hd->hd_type = type;
  hd->hd_seek_to_segment = PTS_UNSET;
  hd->hd_last_dts = PTS_UNSET;
  hd->hd_cancellable = cancellable_create();
}


/**
 *
 */
static void
hls_demuxer_close(media_pipe_t *mp, hls_demuxer_t *hd)
{
  variants_destroy(&hd->hd_variants);
  if(hd->hd_audio_codec != NULL)
    media_codec_deref(hd->hd_audio_codec);
  hls_free_mbp(mp, &hd->hd_mb);
  cancellable_release(hd->hd_cancellable);
}

/**
 *
 */
event_t *
hls_play_extm3u(char *buf, const char *url, media_pipe_t *mp,
		char *errbuf, size_t errlen,
		video_queue_t *vq, struct vsource_list *vsl,
		const video_args_t *va0)
{
  if(!mystrbegins(buf, "#EXTM3U")) {
    snprintf(errbuf, errlen, "Not an m3u file");
    return NULL;
  }

  //usage_event("Play video", 1, USAGE_SEG("format", "HLS"));

  prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 1);

  mp->mp_audio.mq_demuxer_flags = 0;
  mp->mp_video.mq_demuxer_flags = 0;
  mp->mp_subtitle.mq_demuxer_flags = 0;

  hls_t h;
  memset(&h, 0, sizeof(h));
  hls_demuxer_init(&h.h_primary, &h, "primary");
  hls_demuxer_init(&h.h_audio, &h, "audio");
  hls_demuxer_init(&h.h_subtitle, &h, "subtitle");
  h.h_mp = mp;
  h.h_baseurl = url;
  h.h_codec_h264 = NULL;//media_codec_create(AV_CODEC_ID_H264, 1, NULL, NULL, NULL, mp);
  //h.h_codec_mpeg2 = media_codec_create(AV_CODEC_ID_MPEG2VIDEO, 1, NULL, NULL, NULL, mp);
  h.h_debug = gconf.enable_hls_debug;
  int pid = 0;

  if(strstr(buf, "#EXT-X-STREAM-INF:")) {

    hls_variant_t *hv = NULL;
    int first_variant = 1;

    LINEPARSE(s, buf) {
      const char *v;
      if((v = mystrbegins(s, "#EXT-X-MEDIA:")) != NULL)
        pid = hls_ext_x_media(&h, v, pid);
      else if((v = mystrbegins(s, "#EXT-X-STREAM-INF:")) != NULL)
	  {
        hls_ext_x_stream_inf(&h, v, &hv, &h.h_primary);
	  }
	      else if(s[0] != '#') { // && hv->hv_codec_avc
		if(hv == NULL) {
		  HLS_TRACE(&h, "Ignoring variant URI without EXT-X-STREAM-INF: %s",
		            s);
		  continue;
		}
		    if(video_settings.hls_limit!=0 && video_settings.hls_limit!=6 && video_settings.hls_limit!=12) // no limit / no limit (avc) / AVC/HEVC Only
		{
			if(
				//SD
				((video_settings.hls_limit == 1 || video_settings.hls_limit ==  7 || video_settings.hls_limit ==  13) && !(hv->hv_width > gconf.hls_limit_sd || hv->hv_height > 480) ) ||
				//720p
				((video_settings.hls_limit == 2 || video_settings.hls_limit ==  8 || video_settings.hls_limit ==  14) && !(hv->hv_width > 1280 || hv->hv_height > 720) ) ||
				//1080p
				((video_settings.hls_limit == 3 || video_settings.hls_limit ==  9 || video_settings.hls_limit ==  15) && !(hv->hv_width > 1920 || hv->hv_height > 1080) ) ||
				//1440p
				((video_settings.hls_limit == 4 || video_settings.hls_limit == 10 || video_settings.hls_limit ==  16) && !(hv->hv_width > 2560 || hv->hv_height > 1440) ) ||
				//4K
				((video_settings.hls_limit == 5 || video_settings.hls_limit == 11 || video_settings.hls_limit ==  17) && !(hv->hv_width > 3840 || hv->hv_height > 2160) )
			)
			{
				hv->hv_initial = first_variant;
				first_variant = 0;
				if(hls_add_variant(&h, s, hv, &h.h_primary, NULL) != NULL)
					hls_get_video_track(&h, hv);
				hv = NULL;
			}
			else
			{
				hv->hv_initial = 0;
				if(hls_add_variant(&h, s, hv, &h.h_primary, NULL) != NULL)
					hls_get_video_track(&h, hv);
				hv = NULL;
			}
		}
		else
		{
			hv->hv_initial = first_variant;
			first_variant = 0;
			if(hls_add_variant(&h, s, hv, &h.h_primary, NULL) != NULL)
				hls_get_video_track(&h, hv);
			hv = NULL;
		}
      }
    }

    if(hv != NULL)
      variant_destroy(hv);

  } else {
    hls_add_variant(&h, h.h_baseurl, NULL, &h.h_primary, "single");
  }


  check_audio_only(&h.h_primary);

  hls_dump(&h);

  event_t *e = hls_play(&h, mp, errbuf, errlen, va0);

  hls_demuxer_close(mp, &h.h_primary);
  hls_demuxer_close(mp, &h.h_audio);
  hls_demuxer_close(mp, &h.h_subtitle);

  //media_codec_deref(h.h_codec_h264);

  hls_free_audio_tracks(&h);
  hls_free_subtitle_tracks(&h);
  assert(LIST_FIRST(&h.h_discontinuity_segments) == NULL);

  HLS_TRACE(&h, "HLS player done");

  return e;
}


/**
 *
 */
static event_t *
hls_playvideo(const char *url, media_pipe_t *mp,
              char *errbuf, size_t errlen,
              video_queue_t *vq, struct vsource_list *vsl,
              const video_args_t *va0)
{
  buf_t *buf;

  mp_set_url(mp, va0->canonical_url, va0->parent_url, va0->parent_title);

#if LAST_ACTION
	f_last_action = arch_get_avtime();
#endif
  prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 1);

  url += strlen("hls:");
  //if(!strcmp(url, "test"))
  //  url = TESTURL;

  char *baseurl = NULL;

  buf = fa_load(url,
                FA_LOAD_ERRBUF(errbuf, errlen),
                FA_LOAD_FLAGS(FA_COMPRESSION),
                FA_LOAD_CANCELLABLE(mp->mp_cancellable),
                FA_LOAD_LOCATION(&baseurl),
                NULL);
  if(buf == NULL) {
    free(baseurl);
    return NULL;
  }
  buf = buf_make_writable(buf);
  char *s = buf_str(buf);
  event_t *e;

  mp->mp_reset_time = va0->load_request_timestamp;
  mp->mp_reset_epoch = mp->mp_epoch;

  if(s == NULL) {
    snprintf(errbuf, errlen, "Playlist contains no data");
    e = NULL;
  } else {
    e = hls_play_extm3u(s, baseurl, mp, errbuf, errlen, vq, vsl, va0);
  }
  buf_release(buf);
  free(baseurl);

#if LAST_ACTION
	f_last_action = arch_get_avtime();
#endif
  return e;
}


/**
 *
 */
static int
hls_canhandle(const char *url)
{
  return !strncmp(url, "hls:", strlen("hls:"));
}

/**
 *
 */
static int
hls_open(prop_t *page, const char *url, int sync)
{
  //usage_page_open(sync, "HLS");
  return backend_open_video(page, url, sync);
}


/**
 *
 */
static backend_t be_hls = {
  .be_canhandle = hls_canhandle,
  .be_open = hls_open,
  .be_play_video = hls_playvideo,
};

BE_REGISTER(hls);
