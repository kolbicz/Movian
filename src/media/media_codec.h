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
#pragma once
#include <libavfilter/avfilter.h>
struct AVCodecContext;
struct video_decoder;

/**
 *
 */
typedef struct media_codec {
  atomic_t refcount;
  struct media_format *fw;
  int codec_id;

  struct AVCodecContext *fmt_ctx;     // Context owned by AVFormatContext
  struct AVCodecContext *ctx;         // Context owned by decoder thread

  struct AVCodecParserContext *parser_ctx;

  void *opaque;

  struct media_pipe *mp;

  void (*decode)(struct media_codec *mc, struct video_decoder *vd,
		 struct media_queue *mq, struct media_buf *mb, int reqsize);

  int (*decode_locked)(struct media_codec *mc, struct video_decoder *vd,
                       struct media_queue *mq, struct media_buf *mb);

  void (*flush)(struct media_codec *mc, struct video_decoder *vd);

  void (*close)(struct media_codec *mc);
  void (*reinit)(struct media_codec *mc);
  void (*reconfigure)(struct media_codec *mc, const struct frame_info *fi);

  unsigned int sar_num;
  unsigned int sar_den;

  int (*get_buffer2)(struct AVCodecContext *s, AVFrame *frame, int flags);

  AVFilterContext *filt_filter_src_ctx;
  AVFilterContext *filt_filter_sink_ctx;
  AVFilterGraph *filt_filter_graph;
  AVFrame *filt_filter_frame;
  AVFilterInOut *filt_inputs;
  AVFilterInOut *filt_outputs;

} media_codec_t;

struct AVFormatContext;
struct AVCodecContext;
struct media_format;

/**
 *
 */
typedef struct media_codec_params {
  const void *extradata;
  size_t extradata_size;

  unsigned int width;
  unsigned int height;
  int profile;
  int level;
  int pixel_format;
  int bits_per_component;
  int color_primaries;
  int color_transfer;
  int color_matrix;
  int color_range;
  float hdr_mastering_max_luminance;
  unsigned int hdr_max_cll;
  unsigned int hdr_max_fall;
  unsigned int codec_tag;
  int dovi_valid;
  uint8_t dovi_version_major;
  uint8_t dovi_version_minor;
  uint8_t dovi_profile;
  uint8_t dovi_level;
  uint8_t dovi_rpu_present;
  uint8_t dovi_el_present;
  uint8_t dovi_bl_present;
  uint8_t dovi_bl_compatibility_id;
  int cheat_for_speed : 1;
  int broken_aud_placement : 1;
  /* Internal Apple decoder hint: extradata was synthesized from an MPEG-TS
   * HEVC Annex-B access unit and should be validated from its parameter sets
   * rather than treated as a container-owned hvcC atom. */
  unsigned int hevc_annexb_config : 1;
  unsigned int sar_num;
  unsigned int sar_den;

  unsigned int frame_rate_num;
  unsigned int frame_rate_den;

} media_codec_params_t;


/**
 *
 */
typedef struct codec_def {
  LIST_ENTRY(codec_def) link;
  void (*init)(void);
  int (*open)(media_codec_t *mc, const media_codec_params_t *mcp,
	      struct media_pipe *mp);
  int prio;
} codec_def_t;

void media_register_codec(codec_def_t *cd);

// Higher value of prio_ == better preference

#define REGISTER_CODEC(init_, open_, prio_)			   \
  static codec_def_t HTS_JOIN(codecdef, __LINE__) = {		   \
    .init = init_,						   \
    .open = open_,						   \
    .prio = prio_						   \
  };								   \
  INITIALIZER(HTS_JOIN(registercodecdef, __LINE__))                \
  { media_register_codec(&HTS_JOIN(codecdef, __LINE__)); }


/**
 *
 */
typedef struct media_format {
  atomic_t refcount;
  struct AVFormatContext *fctx;
} media_format_t;

#if ENABLE_LIBAV

media_format_t *media_format_create(struct AVFormatContext *fctx);

void media_format_deref(media_format_t *fw);

#endif

/**
 * Codecs
 */
void media_codec_deref(media_codec_t *cw);

media_codec_t *media_codec_ref(media_codec_t *cw);

media_codec_t *media_codec_create(int codec_id, int parser,
				  struct media_format *fw,
				  struct AVCodecContext *ctx,
				  const media_codec_params_t *mcp,
                                  struct media_pipe *mp);

void media_codec_init(void);
