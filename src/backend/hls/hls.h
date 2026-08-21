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
#include "subtitles/video_overlay.h"

event_t *hls_play_extm3u(char *s, const char *url, media_pipe_t *mp,
			 char *errbuf, size_t errlen,
			 video_queue_t *vq, struct vsource_list *vsl,
			 const video_args_t *va0);




#define HLS_QUEUE_MERGE         0x1
#define HLS_QUEUE_KEYFRAME_SEEN 0x2

#define HLS_EOF  ((void *)-1)  // End-of-file
#define HLS_NYA  ((void *)-2)  // Not-yet available
#define HLS_DIS  ((void *)-3)  // Discontinuity


TAILQ_HEAD(hls_variant_queue, hls_variant);
TAILQ_HEAD(hls_segment_queue, hls_segment);
LIST_HEAD(hls_audio_track_list, hls_audio_track);
LIST_HEAD(hls_subtitle_track_list, hls_subtitle_track);

#define HLS_CRYPTO_NONE   0
#define HLS_CRYPTO_AES128 1

#define HLS_MAP_NONE   	  0
#define HLS_MAP_FMP4	  1

/**
 *
 */
typedef struct hls_discontinuity_segment {
  LIST_ENTRY(hls_discontinuity_segment) hds_link;
  int hds_seq;
  int hds_refcount;
  int64_t hds_offset;
} hls_discontinuity_segment_t;


/**
 *
 */
typedef struct hls_segment {
  TAILQ_ENTRY(hls_segment) hs_link;
  char *hs_url;
  int hs_size;
  int64_t hs_byte_offset;
  int64_t hs_byte_size;
  int64_t hs_duration; // in usec

  int64_t hs_time_offset;
  int64_t hs_ts_offset;

  int hs_seq;
  hls_discontinuity_segment_t *hs_discontinuity_segment;

  uint8_t hs_crypto;
  uint8_t hs_map;
  uint8_t hs_open_error;

  rstr_t *hs_key_url;
  rstr_t *hs_map_url;
  uint8_t hs_iv[16];

  struct hls_variant *hs_variant;

  char hs_permanent_error;
  char hs_mark;

  fa_handle_t *hs_fh;

  int64_t hs_open_time;
  int64_t hs_opened_time;
  int64_t hs_first_byte_time;
  int64_t hs_last_byte_time;
  int hs_blocked_counter;

} hls_segment_t;


typedef enum {
  HLS_ERROR_OK = 0,
  HLS_ERROR_SEGMENT_NOT_FOUND,
  HLS_ERROR_SEGMENT_BROKEN,
  HLS_ERROR_SEGMENT_ACCESS_DENIED,
  HLS_ERROR_SEGMENT_BAD_KEY,
  HLS_ERROR_VARIANT_PROBE_ERROR,
  HLS_ERROR_VARIANT_NO_VIDEO,
  HLS_ERROR_VARIANT_UNKNOWN_AUDIO,
  HLS_ERROR_VARIANT_EMPTY,
  HLS_ERROR_VARIANT_NOT_FOUND,
  HLS_ERROR_VARIANT_UNKNOWN_SUBTITLE,
} hls_error_t;


/**
 *
 */
typedef struct hls_variant {
  TAILQ_ENTRY(hls_variant) hv_link;
  char *hv_url;
  struct hls_demuxer *hv_demuxer;
  int64_t hv_byte_counter;

  void *hv_demuxer_private;
  void (*hv_demuxer_close)(struct hls_variant *);
  void (*hv_demuxer_flush)(struct hls_variant *);

  int hv_vstream;
  int hv_astream;

  int hv_last_seq;
  int hv_first_seq;

  struct hls_segment_queue hv_segments;
  struct hls_segment *hv_segment_search;

  char hv_frozen;
  char hv_audio_only;
  char hv_initial;

  int hv_h264_profile;
  int hv_h264_level;
  int hv_codec_hdr;
  int hv_codec;
  /*
  int hv_codec_avc;
  int hv_codec_av1;
  int hv_codec_hvc;
  int hv_codec_vp9;
  int hv_codec_mpg;
  */
  int hv_target_duration;

  time_t hv_loaded; /* last time it was loaded successfully
                     *  0 means not loaded
                     */
  int hv_program;
  int hv_bitrate;
  int hv_default;

  int hv_width;
  int hv_height;
  float hv_fps;
  int hv_sar_num;
  int hv_sar_den;
  int hv_field_order;

  int hv_corrupt_counter;
  int hv_corruptions_last_period;
  int64_t hv_corrupt_timer; // When period above started

  char *hv_subs_group;
  char *hv_audio_group;

  int64_t hv_duration;

  hls_segment_t *hv_current_seg;

  int hv_opening_file;

  rstr_t *hv_key_url;
  buf_t *hv_key;
  //rstr_t *hv_map_url;
  //buf_t *hv_map;


  uint8_t *hv_video_headers;
  int hv_video_headers_size;

  hls_segment_t *hv_last_pos_search;

  int hv_continuity_counter;

  char hv_name[32];

  int hv_audio_stream;
  int hv_subtitle_stream;
  //int64_t hv_mpeg_ts_last;

  int64_t hv_start_time_offset;

} hls_variant_t;


/**
 *
 */
typedef struct hls_demuxer {
  struct hls_variant_queue hd_variants;
  const char *hd_type;

  hls_variant_t *hd_current;
  hls_variant_t *hd_req;

  int hd_bw;
  int hd_bw_updated;
  //int64_t hd_download_counter_reset_at;
  //int64_t hd_download_counter;
  //int64_t hd_download_counter2;
  int hd_download_blocked;

  int hd_current_stream;
  int hd_pending_stream;

  media_codec_t *hd_audio_codec;

  int64_t hd_last_switch;

  cancellable_t *hd_cancellable;

  struct hls *hd_hls;

  // When set, nothing seems to be working, bail out
  int hd_no_functional_streams;

  /** Used to find segment to seek to, will be reset to PTS_UNSET
   * once the segment has been found. After the correct segment has
   * been found we rely on mq_seek_target mark frames as skipped until
   * we reach the final seek point
   */
  int64_t hd_seek_to_segment;

  media_buf_t *hd_mb;

  int hd_discontinuity;
  int hd_discontinuity_seq;

  int64_t hd_last_dts;

} hls_demuxer_t;

LIST_HEAD(hls_discontinuity_segment_list, hls_discontinuity_segment);

/**
 *
 */
typedef struct hls {
  const char *h_baseurl;

  int h_debug;

  media_pipe_t *h_mp;

  media_codec_t *h_codec_h264;

/*
  int32_t time_bas;
  int32_t time_div;
  int time_ok;
  int32_t time_den;
  int32_t time_num;
*/

  int h_blocked;

  hls_demuxer_t h_primary;
  hls_demuxer_t h_audio;
  hls_demuxer_t h_subtitle;

  int h_playback_priority;

  int h_restartpos_last;
  int64_t h_last_timestamp_presented;
  char h_sub_scanning_done;
  char h_enqueued_something;

  int64_t h_duration;  // Total duration (if known)

  struct hls_audio_track_list h_audio_tracks;
  struct hls_subtitle_track_list h_subtitle_tracks;

  int h_last_enqueued_seq;
  int h_last_decoded_sub;

  struct hls_discontinuity_segment_list h_discontinuity_segments;

  hls_error_t h_last_error;

} hls_t;

int hls_segment_read(hls_segment_t *hs, void *buf, size_t size);



typedef struct hls_audio_track {
  LIST_ENTRY(hls_audio_track) hat_link;
  int hat_stream_id;
  int hat_pid;
  char *hat_mux_id;
  prop_t *hat_trackprop;
} hls_audio_track_t;

typedef struct hls_subtitle_track {
  LIST_ENTRY(hls_subtitle_track) hat_link;
  int hat_stream_id;
  int hat_pid;
  char *hat_mux_id;
  prop_t *hat_trackprop;
} hls_subtitle_track_t;

#define HLS_TRACE(h, x, ...) do {                               \
    if((h)->h_debug)                                            \
      TRACE(TRACE_DEBUG, "HLS", x, ##__VA_ARGS__);		\
  } while(0)

hls_error_t hls_segment_open(hls_segment_t *hs);

void hls_segment_close(hls_segment_t *hs);

void hls_variant_open(hls_variant_t *hv);

void hls_variant_close(hls_variant_t *hv);

//void hls_check_bw_switch(hls_demuxer_t *hd);

//int hls_bw_switch(hls_demuxer_t *hd, int bw);

void hls_get_max_width_height(hls_demuxer_t *hd, int *wd, int *hg);

hls_variant_t *hls_select_default_variant(hls_demuxer_t *hd);


hls_segment_t *hls_variant_select_next_segment(hls_variant_t *hv);

int hls_get_audio_track(hls_t *h, int pid, const char *mux_id, const char *language,
                    const char *fmt, const char *chans, int autosel);

int hls_get_subtitle_track(hls_t *h, int pid, const char *name,
                        const char *language,
                        const char *fmt, int autosel);

hls_segment_t *hv_find_segment_by_seq(hls_variant_t *hv, int seq);

void hls_bad_variant(hls_variant_t *hv, hls_error_t err);

// TS demuxer

media_buf_t *hls_ts_demuxer_read(hls_demuxer_t *hd);

typedef struct ext_subtitles {
  struct video_overlay_queue es_entries;

} ext_subtitles_t;

/**
 *
 */
static const char *hlserrstr[] = {
  [HLS_ERROR_SEGMENT_NOT_FOUND]     = "Segment not found",
  [HLS_ERROR_SEGMENT_BROKEN]        = "Unable to open segment",
  [HLS_ERROR_SEGMENT_ACCESS_DENIED] = "Access denied",
  [HLS_ERROR_SEGMENT_BAD_KEY]       = "Unable to get encryption key",
  [HLS_ERROR_VARIANT_PROBE_ERROR]   = "Probing error",
  [HLS_ERROR_VARIANT_NO_VIDEO]      = "No video",
  [HLS_ERROR_VARIANT_UNKNOWN_AUDIO] = "Unknown audio codec",
  [HLS_ERROR_VARIANT_UNKNOWN_SUBTITLE] = "Unknown subtitle codec",
  [HLS_ERROR_VARIANT_EMPTY]         = "Variant is empty",
  [HLS_ERROR_VARIANT_NOT_FOUND]     = "Variant not found",
};
