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
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFString.h>

#include <VideoToolbox/VideoToolbox.h>

#include "main.h"
#include "media/media.h"
#include "video_decoder.h"
#include "video_settings.h"
#include "h264_annexb.h"

#if TARGET_OS_OSX || TARGET_OS_IPHONE
#include "../../ext/libav/libavcodec/cbs.h"
#include "../../ext/libav/libavcodec/cbs_av1.h"
#endif


LIST_HEAD(vtb_frame_list, vtb_frame);

/**
 *
 */
typedef struct vtb_frame {
  LIST_ENTRY(vtb_frame) vf_link;
  CVPixelBufferRef vf_buf;
  media_buf_meta_t vf_mbm;
} vtb_frame_t;



/**
 *
 */
typedef struct vtb_decoder {
  VTDecompressionSessionRef vtbd_session;
  CMVideoFormatDescriptionRef vtbd_fmt;
  VTPixelTransferSessionRef vtbd_pixel_transfer;
  CVPixelBufferPoolRef vtbd_sdr_pool;

  hts_mutex_t vtbd_mutex;
  video_decoder_t *vtbd_vd;

  struct vtb_frame_list vtbd_frames;
  int64_t vtbd_max_ts;
  int64_t vtbd_flush_to;
  int64_t vtbd_last_pts;
  int vtbd_estimated_duration;
  int vtbd_pixel_format;
  int vtbd_decode_pixel_format;
  int vtbd_codec_id;
  int vtbd_profile;
  int vtbd_color_transfer;
  int vtbd_hdr_to_sdr;
  int vtbd_assumed_sdr;
  int vtbd_p010_playback;
  int vtbd_p010_direct;
  int vtbd_dolby_vision_tag;
  int vtbd_hw_status_reported;
  int vtbd_output_reported;
  int vtbd_sdr_output_reported;
} vtb_decoder_t;

static void dict_set_int32(CFMutableDictionaryRef dict, CFStringRef key,
                           int value);

#if TARGET_OS_OSX || TARGET_OS_IPHONE
typedef struct av1_config_info {
  int profile;
  int level;
  int tier;
  int depth;
  int monochrome;
  int subsampling_x;
  int subsampling_y;
  int chroma_sample_position;
} av1_config_info_t;

static int
av1_parse_config(const media_codec_params_t *mcp, av1_config_info_t *info)
{
  CodedBitstreamContext *ctx = NULL;
  CodedBitstreamFragment frag = {0};
  AVCodecParameters par = {0};
  int err = ff_cbs_init(&ctx, AV_CODEC_ID_AV1, NULL);
  if(err < 0)
    return err;

  par.codec_type = AVMEDIA_TYPE_VIDEO;
  par.codec_id = AV_CODEC_ID_AV1;
  par.extradata = (uint8_t *)mcp->extradata;
  par.extradata_size = mcp->extradata_size;
  err = ff_cbs_read_extradata(ctx, &frag, &par);
  if(err < 0)
    goto done;

  err = AVERROR_INVALIDDATA;
  for(int i = 0; i < frag.nb_units; i++) {
    CodedBitstreamUnit *unit = &frag.units[i];
    if(unit->type != AV1_OBU_SEQUENCE_HEADER || unit->content == NULL)
      continue;

    const AV1RawOBU *obu = unit->content;
    const AV1RawSequenceHeader *seq = &obu->obu.sequence_header;
    const AV1RawColorConfig *color = &seq->color_config;
    info->profile = seq->seq_profile;
    info->level = seq->seq_level_idx[0];
    info->tier = seq->seq_tier[0];
    info->depth = color->high_bitdepth ?
      (color->twelve_bit ? 12 : 10) : 8;
    info->monochrome = color->mono_chrome;
    info->subsampling_x = color->subsampling_x;
    info->subsampling_y = color->subsampling_y;
    info->chroma_sample_position = color->chroma_sample_position;
    err = 0;
    break;
  }

done:
  ff_cbs_fragment_reset(&frag);
  ff_cbs_close(&ctx);
  return err;
}
#endif

static const char *
vtb_codec_name(enum AVCodecID codec_id)
{
  switch(codec_id) {
  case AV_CODEC_ID_H264:
    return "H264";
  case AV_CODEC_ID_HEVC:
    return "HEVC";
  case AV_CODEC_ID_AV1:
    return "AV1";
  case AV_CODEC_ID_VP9:
    return "VP9";
  default:
    return "unknown";
  }
}

static void
add_source_color_extensions(CFMutableDictionaryRef dict,
                            const media_codec_params_t *mcp)
{
  if(mcp->color_primaries == AVCOL_PRI_BT2020)
    CFDictionarySetValue(dict, kCVImageBufferColorPrimariesKey,
                         kCVImageBufferColorPrimaries_ITU_R_2020);

  if(mcp->color_transfer == AVCOL_TRC_SMPTE2084)
    CFDictionarySetValue(dict, kCVImageBufferTransferFunctionKey,
                         kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
  else if(mcp->color_transfer == AVCOL_TRC_ARIB_STD_B67)
    CFDictionarySetValue(dict, kCVImageBufferTransferFunctionKey,
                         kCVImageBufferTransferFunction_ITU_R_2100_HLG);

  if(mcp->color_matrix == AVCOL_SPC_BT2020_NCL ||
     mcp->color_matrix == AVCOL_SPC_BT2020_CL)
    CFDictionarySetValue(dict, kCVImageBufferYCbCrMatrixKey,
                         kCVImageBufferYCbCrMatrix_ITU_R_2020);
}


/**
 * Main10 SDR can be decoded directly into the existing 8-bit NV12 renderer.
 * Explicit PQ and HLG use VideoToolbox's HDR-to-SDR pixel-transfer stage;
 * unspecified transfer metadata stays on the conservative fallback path.
 */
static int
hevc_main10_is_sdr(const media_codec_params_t *mcp)
{
  switch(mcp->color_transfer) {
  case AVCOL_TRC_BT709:
  case AVCOL_TRC_GAMMA22:
  case AVCOL_TRC_GAMMA28:
  case AVCOL_TRC_SMPTE170M:
  case AVCOL_TRC_SMPTE240M:
  case AVCOL_TRC_BT2020_10:
  case AVCOL_TRC_BT2020_12:
    return 1;
  default:
    return 0;
  }
}


/**
 * HDR formats that VideoToolbox can safely convert to the existing BT.709,
 * 8-bit renderer through its pixel-transfer stage.  Unknown transfer metadata
 * remains on the software path because treating it as either SDR or HDR can
 * produce incorrect luminance.
 */
static int
hevc_main10_is_hdr(const media_codec_params_t *mcp)
{
  return mcp->color_transfer == AVCOL_TRC_SMPTE2084 ||
         mcp->color_transfer == AVCOL_TRC_ARIB_STD_B67;
}


/**
 * Return a printable representation of a CoreVideo color attachment.
 */
static const char *
copy_attachment_string(CVBufferRef buf, CFStringRef key,
                       char *dst, size_t dstlen)
{
  CFTypeRef value = CVBufferCopyAttachment(buf, key, NULL);
  if(value == NULL) {
    snprintf(dst, dstlen, "missing");
  } else if(CFGetTypeID(value) != CFStringGetTypeID() ||
            !CFStringGetCString(value, dst, dstlen, kCFStringEncodingUTF8)) {
    snprintf(dst, dstlen, "non-string");
  }
  if(value != NULL)
    CFRelease(value);
  return dst;
}


/**
 * Log the real output selected by VideoToolbox.  Session creation only states
 * what Movian requested; these first-frame values tell us what each iOS/macOS
 * version and device actually returned after HDR pixel transfer.
 */
static void
report_output_format(vtb_decoder_t *vtbd, CVPixelBufferRef imageBuffer,
                     const char *stage)
{
  const OSType pf = CVPixelBufferGetPixelFormatType(imageBuffer);
  char primaries[96], transfer[96], matrix[96];

  TRACE(TRACE_INFO, "VTB",
        "%s output buffer format=%c%c%c%c (0x%08x), planes=%zu, IOSurface=%s, primaries=%s, transfer=%s, matrix=%s%s",
        stage,
        (int)((pf >> 24) & 0xff), (int)((pf >> 16) & 0xff),
        (int)((pf >> 8) & 0xff), (int)(pf & 0xff), (unsigned int)pf,
        CVPixelBufferGetPlaneCount(imageBuffer),
        CVPixelBufferGetIOSurface(imageBuffer) != NULL ? "yes" : "no",
        copy_attachment_string(imageBuffer, kCVImageBufferColorPrimariesKey,
                               primaries, sizeof(primaries)),
        copy_attachment_string(imageBuffer, kCVImageBufferTransferFunctionKey,
                               transfer, sizeof(transfer)),
        copy_attachment_string(imageBuffer, kCVImageBufferYCbCrMatrixKey,
                               matrix, sizeof(matrix)),
        vtbd->vtbd_dolby_vision_tag ? ", Dolby-Vision-tag=yes" : "");
}


/**
 * Test whether VideoToolbox accepts a hardware HEVC session requesting P010.
 * The probe session never decodes or renders frames, so the working NV12 path
 * remains unchanged.  This is capability evidence for a future renderer.
 */
static void
probe_p010_output(CMVideoFormatDescriptionRef fmt,
                  CFDictionaryRef decoder_specification,
                  int width, int height)
{
  CFMutableDictionaryRef attrs =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 4,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  dict_set_int32(attrs, kCVPixelBufferWidthKey, width);
  dict_set_int32(attrs, kCVPixelBufferHeightKey, height);
  dict_set_int32(attrs, kCVPixelBufferPixelFormatTypeKey,
                 kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

  CFMutableDictionaryRef iosurface_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey,
                       iosurface_dict);
  CFRelease(iosurface_dict);

  VTDecompressionSessionRef probe_session = NULL;
  OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt,
                                                  decoder_specification,
                                                  attrs, NULL,
                                                  &probe_session);
  CFRelease(attrs);

  if(status == noErr && probe_session != NULL) {
    TRACE(TRACE_INFO, "VTB",
          "P010 probe: hardware decoder session accepted 10-bit bi-planar IOSurface output");
    VTDecompressionSessionInvalidate(probe_session);
    CFRelease(probe_session);
  } else {
    TRACE(TRACE_INFO, "VTB",
          "P010 probe: decoder session rejected 10-bit output (status=%d)",
          (int)status);
  }
}


#if TARGET_OS_OSX || TARGET_OS_IPHONE
/**
 * VideoToolbox emits real P010 frames; VTPixelTransfer converts them to the
 * BT.709 buffers accepted by the existing platform renderer. Keeping this
 * separate from decompression lets us verify the actual 10-bit output before
 * conversion and avoids adding a P010 OpenGL ES renderer prematurely.
 */
static int
create_p010_sdr_bridge(vtb_decoder_t *vtbd, int width, int height)
{
  OSStatus status = VTPixelTransferSessionCreate(kCFAllocatorDefault,
                                                  &vtbd->vtbd_pixel_transfer);
  if(status != noErr)
    return status;

  CFMutableDictionaryRef transfer_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 3,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(transfer_dict,
                       kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                       kCVImageBufferColorPrimaries_ITU_R_709_2);
  CFDictionarySetValue(transfer_dict,
                       kVTPixelTransferPropertyKey_DestinationTransferFunction,
                       kCVImageBufferTransferFunction_ITU_R_709_2);
  CFDictionarySetValue(transfer_dict,
                       kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                       kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  status = VTSessionSetProperties(vtbd->vtbd_pixel_transfer, transfer_dict);
  CFRelease(transfer_dict);
  if(status != noErr)
    return status;

  CFMutableDictionaryRef pool_attrs =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 3,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  dict_set_int32(pool_attrs, kCVPixelBufferWidthKey, width);
  dict_set_int32(pool_attrs, kCVPixelBufferHeightKey, height);
  dict_set_int32(pool_attrs, kCVPixelBufferPixelFormatTypeKey,
#if TARGET_OS_IPHONE
                 kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
  CFDictionarySetValue(pool_attrs, kCVPixelBufferOpenGLESCompatibilityKey,
                       kCFBooleanTrue);
  CFMutableDictionaryRef iosurface_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(pool_attrs, kCVPixelBufferIOSurfacePropertiesKey,
                       iosurface_dict);
  CFRelease(iosurface_dict);
#else
                 kCVPixelFormatType_420YpCbCr8Planar);
#endif
  CVReturn cvstatus = CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL,
                                               pool_attrs,
                                               &vtbd->vtbd_sdr_pool);
  CFRelease(pool_attrs);
  return cvstatus;
}
#endif


/**
 *
 */
static void
destroy_frame(vtb_frame_t *vf)
{
  CFRelease(vf->vf_buf);
  LIST_REMOVE(vf, vf_link);
  free(vf);
}


/**
 *
 */
static void
destroy_frames(vtb_decoder_t *vtbd)
{
  vtb_frame_t *vf;
  while((vf = LIST_FIRST(&vtbd->vtbd_frames)) != NULL)
    destroy_frame(vf);
}


/**
 *
 */
static void
emit_frame(vtb_decoder_t *vtbd, vtb_frame_t *vf, media_queue_t *mq)
{
  CGSize siz;

  frame_info_t fi;
  memset(&fi, 0, sizeof(fi));

  if(vtbd->vtbd_last_pts != PTS_UNSET && vf->vf_mbm.mbm_pts != PTS_UNSET) {
    int64_t d = vf->vf_mbm.mbm_pts - vtbd->vtbd_last_pts;

    if(d > 1000 && d < 1000000)
      vtbd->vtbd_estimated_duration = d;
  }

  siz = CVImageBufferGetDisplaySize(vf->vf_buf);
  fi.fi_dar_num = siz.width;
  fi.fi_dar_den = siz.height;

  fi.fi_pts = vf->vf_mbm.mbm_pts;
  fi.fi_color_space = vtbd->vtbd_hdr_to_sdr ? COLOR_SPACE_BT_709 : -1;
  fi.fi_color_transfer = vtbd->vtbd_color_transfer;
  fi.fi_epoch = vf->vf_mbm.mbm_epoch;
  fi.fi_drive_clock = vf->vf_mbm.mbm_drive_clock;
  fi.fi_user_time = vf->vf_mbm.mbm_user_time;
  fi.fi_vshift = 1;
  fi.fi_hshift = 1;
  fi.fi_duration = vf->vf_mbm.mbm_duration > 10000 ? vf->vf_mbm.mbm_duration : vtbd->vtbd_estimated_duration;

  siz = CVImageBufferGetEncodedSize(vf->vf_buf);
  fi.fi_width = siz.width;
  fi.fi_height = siz.height;


  video_decoder_t *vd = vtbd->vtbd_vd;
  vd->vd_estimated_duration = fi.fi_duration; // For bitrate calculations

  switch(vtbd->vtbd_pixel_format) {
    case kCVPixelFormatType_420YpCbCr8Planar:
      fi.fi_type = 'YUVP';

      CVPixelBufferLockBaseAddress(vf->vf_buf, 0);

      for(int i = 0; i < 3; i++ ) {
        fi.fi_data[i]  = CVPixelBufferGetBaseAddressOfPlane(vf->vf_buf, i);
        fi.fi_pitch[i] = CVPixelBufferGetBytesPerRowOfPlane(vf->vf_buf, i);
      }

      if(fi.fi_duration > 0)
        video_deliver_frame(vd, &fi);

      CVPixelBufferUnlockBaseAddress(vf->vf_buf, 0);
      break;

    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
      fi.fi_type = 'CVPB';
      fi.fi_data[0] = (void *)vf->vf_buf;
      if(fi.fi_duration > 0)
        video_deliver_frame(vd, &fi);
      break;

    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
      fi.fi_type = 'P010';
      fi.fi_data[0] = (void *)vf->vf_buf;
      if(fi.fi_duration > 0)
        video_deliver_frame(vd, &fi);
      break;
  }



  vtbd->vtbd_last_pts = vf->vf_mbm.mbm_pts;

  char fmt[64];
  snprintf(fmt, sizeof(fmt), "%s (VTB) %d x %d",
           vtbd->vtbd_codec_id == AV_CODEC_ID_HEVC ?
             (vtbd->vtbd_p010_direct ? "HEVC P010 direct" :
              vtbd->vtbd_p010_playback ? "HEVC P010->SDR" :
              vtbd->vtbd_hdr_to_sdr ? "HEVC HDR->SDR" :
              vtbd->vtbd_assumed_sdr ? "HEVC Main10 SDR assumed" :
              vtbd->vtbd_profile == 2 ? "HEVC Main10->8-bit" : "HEVC") :
           vtb_codec_name(vtbd->vtbd_codec_id),
           fi.fi_width, fi.fi_height);
  prop_set_string(mq->mq_prop_codec, fmt);
}



/**
 *
 */
static int
vf_cmp(const vtb_frame_t *a, const vtb_frame_t *b)
{
  if(a->vf_mbm.mbm_epoch < b->vf_mbm.mbm_epoch)
    return -1;
  if(a->vf_mbm.mbm_epoch > b->vf_mbm.mbm_epoch)
    return 1;
  if(a->vf_mbm.mbm_pts < b->vf_mbm.mbm_pts)
    return -1;
  if(a->vf_mbm.mbm_pts > b->vf_mbm.mbm_pts)
    return 1;
  return 0;
}


/**
 *
 */
static void
picture_out(void *decompressionOutputRefCon,
            void *sourceFrameRefCon,
            OSStatus status,
            VTDecodeInfoFlags infoFlags,
            CVPixelBufferRef imageBuffer,
            CMTime pts,
            CMTime duration)
{
  media_buf_meta_t *mbm = sourceFrameRefCon;
  vtb_decoder_t *vtbd = decompressionOutputRefCon;

  if(imageBuffer == NULL)
    return; // No frame, typically from kVTDecodeFrame_DoNotOutputFrame

  if(!vtbd->vtbd_output_reported) {
    report_output_format(vtbd, imageBuffer, "Decoder");
    vtbd->vtbd_output_reported = 1;
  }

  CVPixelBufferRef outputBuffer = imageBuffer;

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  CVPixelBufferRef convertedBuffer = NULL;
  if(vtbd->vtbd_p010_playback && !vtbd->vtbd_p010_direct) {
    hts_mutex_lock(&vtbd->vtbd_mutex);
    CVReturn cvstatus = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                           vtbd->vtbd_sdr_pool,
                                                           &convertedBuffer);
    OSStatus transfer_status = cvstatus == kCVReturnSuccess ?
      VTPixelTransferSessionTransferImage(vtbd->vtbd_pixel_transfer,
                                           imageBuffer, convertedBuffer) :
      cvstatus;
    hts_mutex_unlock(&vtbd->vtbd_mutex);

    if(transfer_status != noErr || convertedBuffer == NULL) {
      TRACE(TRACE_ERROR, "VTB",
            "P010-to-SDR transfer failed (status=%d)",
            (int)transfer_status);
      if(convertedBuffer != NULL)
        CFRelease(convertedBuffer);
      return;
    }
    outputBuffer = convertedBuffer;

    if(!vtbd->vtbd_sdr_output_reported) {
      report_output_format(vtbd, outputBuffer, "SDR bridge");
      vtbd->vtbd_sdr_output_reported = 1;
    }
  }
#endif

  if(!vtbd->vtbd_hw_status_reported) {
    /* Never synchronously query VTSession properties from this callback.
     * On recent macOS VideoToolbox runs the callback on its decoder XPC reply
     * queue; a synchronous VTSessionCopyProperty() here waits on that same
     * service and deadlocks before the first frame can be queued. Session
     * creation used RequireHardwareAcceleratedVideoDecoder, so a successful
     * session already proves that the hardware decoder is active. */
    TRACE(TRACE_INFO, "VTB",
          "Hardware decoder active (required by session; first frame received)");
    vtbd->vtbd_hw_status_reported = 1;
  }

  vtb_frame_t *vf = malloc(sizeof(vtb_frame_t));
  vf->vf_mbm = *mbm;
  vf->vf_buf = outputBuffer;
  CFRetain(outputBuffer);

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  if(convertedBuffer != NULL)
    CFRelease(convertedBuffer);
#endif

  hts_mutex_lock(&vtbd->vtbd_mutex);

  LIST_INSERT_SORTED(&vtbd->vtbd_frames, vf, vf_link, vf_cmp, vtb_frame_t);

  if(vtbd->vtbd_max_ts != PTS_UNSET) {
    if(vf->vf_mbm.mbm_pts > vtbd->vtbd_max_ts) {
      vtbd->vtbd_flush_to = vtbd->vtbd_max_ts;
      vtbd->vtbd_max_ts = vf->vf_mbm.mbm_pts;
    }
  } else {
    vtbd->vtbd_max_ts = vf->vf_mbm.mbm_pts;
  }
  hts_mutex_unlock(&vtbd->vtbd_mutex);
}



/**
 *
 */
static void
vtb_decode(struct media_codec *mc, struct video_decoder *vd,
	   struct media_queue *mq, struct media_buf *mb, int reqsize)
{
  vtb_decoder_t *vtbd = mc->opaque;
  VTDecodeInfoFlags infoflags;
  int flags = kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing;
  OSStatus status;
  CMBlockBufferRef block_buf;
  CMSampleBufferRef sample_buf;

  vtbd->vtbd_vd = vd;

  status =
    CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                       mb->mb_data, mb->mb_size,
                                       kCFAllocatorNull,
                                       NULL, 0, mb->mb_size, 0, &block_buf);
  if(status) {
    TRACE(TRACE_ERROR, "VTB", "Data buffer allocation error %d", status);
    return;
  }

  CMSampleTimingInfo ti;

  ti.duration              = CMTimeMake(mb->mb_duration, 1000000);
  ti.presentationTimeStamp = CMTimeMake(mb->mb_pts, 1000000);
  ti.decodeTimeStamp       = CMTimeMake(mb->mb_dts, 1000000);

  status =
    CMSampleBufferCreate(kCFAllocatorDefault,
                         block_buf, TRUE, 0, 0, vtbd->vtbd_fmt,
                         1, 1, &ti, 0, NULL, &sample_buf);

  CFRelease(block_buf);
  if(status) {
    TRACE(TRACE_ERROR, "VTB", "Sample buffer allocation error %d", status);
    return;
  }

  void *frame_opaque = &vd->vd_reorder[vd->vd_reorder_ptr];
  copy_mbm_from_mb(frame_opaque, mb);
  vd->vd_reorder_ptr = (vd->vd_reorder_ptr + 1) & VIDEO_DECODER_REORDER_MASK;

  if(mb->mb_skip)
    flags |= kVTDecodeFrame_DoNotOutputFrame;

  status =
    VTDecompressionSessionDecodeFrame(vtbd->vtbd_session, sample_buf, flags,
                                      frame_opaque, &infoflags);
  CFRelease(sample_buf);
  if(status) {
    TRACE(TRACE_ERROR, "VTB", "Decoding error %d", status);
  }

  hts_mutex_lock(&vtbd->vtbd_mutex);

  if(vtbd->vtbd_flush_to != PTS_UNSET) {
    vtb_frame_t *vf;
    while((vf = LIST_FIRST(&vtbd->vtbd_frames)) != NULL) {
      if(vtbd->vtbd_flush_to < vf->vf_mbm.mbm_pts)
	break;
      LIST_REMOVE(vf, vf_link);
      hts_mutex_unlock(&vtbd->vtbd_mutex);
      emit_frame(vtbd, vf, mq);
      hts_mutex_lock(&vtbd->vtbd_mutex);
      CFRelease(vf->vf_buf);
      free(vf);
    }
  }
  hts_mutex_unlock(&vtbd->vtbd_mutex);
}


/**
 *
 */
static void
vtb_flush(struct media_codec *mc, struct video_decoder *vd)
{
  vtb_decoder_t *vtbd = mc->opaque;
  VTDecompressionSessionWaitForAsynchronousFrames(vtbd->vtbd_session);
  hts_mutex_lock(&vtbd->vtbd_mutex);
  destroy_frames(vtbd);
  vtbd->vtbd_max_ts   = PTS_UNSET;
  vtbd->vtbd_flush_to = PTS_UNSET;
  vtbd->vtbd_last_pts = PTS_UNSET;
  hts_mutex_unlock(&vtbd->vtbd_mutex);
}


/**
 *
 */
static void
vtb_close(struct media_codec *mc)
{
  vtb_decoder_t *vtbd = mc->opaque;
  VTDecompressionSessionWaitForAsynchronousFrames(vtbd->vtbd_session);
  destroy_frames(vtbd);

  VTDecompressionSessionInvalidate(vtbd->vtbd_session);
  CFRelease(vtbd->vtbd_session);

  if(vtbd->vtbd_pixel_transfer != NULL) {
    VTPixelTransferSessionInvalidate(vtbd->vtbd_pixel_transfer);
    CFRelease(vtbd->vtbd_pixel_transfer);
  }
  if(vtbd->vtbd_sdr_pool != NULL)
    CFRelease(vtbd->vtbd_sdr_pool);

  CFRelease(vtbd->vtbd_fmt);
  free(vtbd);
}


/**
 *
 */
static void
dict_set_int32(CFMutableDictionaryRef dict, CFStringRef key, int value)
{
  CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(dict, key, num);
  CFRelease(num);
}


/**
 *
 */
static int
video_vtb_codec_create(media_codec_t *mc, const media_codec_params_t *mcp,
		       media_pipe_t *mp)
{
  OSStatus status;
  CMVideoCodecType codec_type;
  CFStringRef config_atom = NULL;

  if(!video_settings.video_accel)
    return 1;

  switch(mc->codec_id) {
  case AV_CODEC_ID_H264:
    codec_type = kCMVideoCodecType_H264;
    config_atom = CFSTR("avcC");
    break;
  case AV_CODEC_ID_HEVC:
    codec_type = kCMVideoCodecType_HEVC;
    config_atom = CFSTR("hvcC");
    break;
#if TARGET_OS_OSX || TARGET_OS_IPHONE
  case AV_CODEC_ID_AV1:
    codec_type = kCMVideoCodecType_AV1;
    config_atom = CFSTR("av1C");
    break;
#endif
  case AV_CODEC_ID_VP9:
    codec_type = kCMVideoCodecType_VP9;
    config_atom = CFSTR("vpcC");
    break;
  default:
    return 1;
  }

  if(!VTIsHardwareDecodeSupported(codec_type)) {
    TRACE(TRACE_DEBUG, "VTB", "No hardware decoder for %s",
          vtb_codec_name(mc->codec_id));
    return 1;
  }

  if(mcp == NULL)
    return 1;

  if(mc->codec_id == AV_CODEC_ID_VP9) {
    /* Start with VP9 Profile 0 / 8-bit 4:2:0. Profile may be unknown when a
     * WebM demuxer has not parsed the first frame yet; VideoToolbox remains
     * the authority and a rejected session falls through to libavcodec. */
    if((mcp->profile != FF_PROFILE_UNKNOWN && mcp->profile != 0) ||
       mcp->bits_per_component > 8) {
      TRACE(TRACE_DEBUG, "VTB",
            "VP9 hardware preview rejected profile/depth: profile=%d depth=%d",
            mcp->profile, mcp->bits_per_component);
      return 1;
    }
  }

  if(mcp->extradata == NULL || mcp->extradata_size == 0) {
    if(mc->codec_id == AV_CODEC_ID_HEVC)
      return 1;
    if(mc->codec_id == AV_CODEC_ID_AV1)
      return 1;
    if(mc->codec_id == AV_CODEC_ID_VP9) {
      config_atom = NULL;
    } else
    return h264_annexb_to_avc(mc, mp, &video_vtb_codec_create);
  }

  const uint8_t *codec_config = mcp->extradata;
  size_t codec_config_size = mcp->extradata_size;
  uint8_t *owned_codec_config = NULL;
  if(mc->codec_id != AV_CODEC_ID_AV1 && mc->codec_id != AV_CODEC_ID_VP9 &&
     codec_config[0] != 1) {
    if(mc->codec_id == AV_CODEC_ID_HEVC)
      return 1;
    return h264_annexb_to_avc(mc, mp, &video_vtb_codec_create);
  }

  int av1_depth = 0;

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  /* FFmpeg 4.4 exposes MP4 AV1 extradata as sequence-header OBUs rather than
   * the complete av1C record required by CMVideoFormatDescription. Parse the
   * sequence header and construct an accurate configuration record, including
   * streams for which AVCodecParameters leaves format/depth unspecified. */
  if(mc->codec_id == AV_CODEC_ID_AV1) {
    av1_config_info_t av1;
    if(av1_parse_config(mcp, &av1)) {
      TRACE(TRACE_DEBUG, "VTB", "Unable to parse AV1 sequence header");
      return 1;
    }
    if(av1.profile != FF_PROFILE_AV1_MAIN ||
       (av1.depth != 8 && av1.depth != 10) ||
       av1.monochrome || !av1.subsampling_x || !av1.subsampling_y) {
      TRACE(TRACE_DEBUG, "VTB",
            "AV1 hardware preview rejected format: profile=%d depth=%d monochrome=%d subsampling=%d:%d",
            av1.profile, av1.depth, av1.monochrome,
            av1.subsampling_x, av1.subsampling_y);
      return 1;
    }

    av1_depth = av1.depth;
    owned_codec_config = malloc(codec_config_size + 4);
    if(owned_codec_config == NULL)
      return 1;
    owned_codec_config[0] = 0x81 | ((av1.profile & 7) << 4);
    owned_codec_config[1] = av1.level & 0x1f;
    owned_codec_config[2] = ((av1.tier & 1) << 7) |
                            ((av1.depth > 8) << 6) |
                            ((av1.depth == 12) << 5) |
                            ((av1.monochrome & 1) << 4) |
                            ((av1.subsampling_x & 1) << 3) |
                            ((av1.subsampling_y & 1) << 2) |
                            (av1.chroma_sample_position & 3);
    owned_codec_config[3] = 0;
    memcpy(owned_codec_config + 4, codec_config, codec_config_size);
    codec_config = owned_codec_config;
    codec_config_size += 4;
    TRACE(TRACE_INFO, "VTB",
          "Built AV1 configuration record from sequence header: profile=%d level=%d depth=%d",
          av1.profile, av1.level, av1.depth);
  }
#endif

  int stream_profile = mcp->profile;
  int assumed_sdr = 0;

  if(mc->codec_id == AV_CODEC_ID_HEVC) {
    const uint8_t *hvcC = mcp->extradata;
    if(mcp->extradata_size < 23)
      return 1;

    stream_profile = hvcC[1] & 0x1f;
    if(stream_profile != 1 && stream_profile != 2)
      return 1;

    if(stream_profile == 2 && !hevc_main10_is_sdr(mcp) &&
       !hevc_main10_is_hdr(mcp)) {
      if(mcp->color_transfer == AVCOL_TRC_UNSPECIFIED &&
         video_settings.video_accel_untagged_main10) {
        assumed_sdr = 1;
        TRACE(TRACE_INFO, "VTB",
              "HEVC Main10 SDR assumed: transfer metadata is unspecified and opt-in is enabled");
      } else {
        TRACE(TRACE_INFO, "VTB",
              "HEVC Main10 uses fallback: transfer=%d is unsupported or unspecified%s",
              mcp->color_transfer,
              mcp->color_transfer == AVCOL_TRC_UNSPECIFIED ?
                " (enable 'Assume untagged HEVC Main10 is SDR' to override)" : "");
        return 1;
      }
    }
  }

  CFMutableDictionaryRef config_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault,
                              2,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(config_dict,
                       kCVImageBufferChromaLocationBottomFieldKey,
                       kCVImageBufferChromaLocation_Left);

  CFDictionarySetValue(config_dict,
                       kCVImageBufferChromaLocationTopFieldKey,
                       kCVImageBufferChromaLocation_Left);

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  if(((mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2) ||
      (mc->codec_id == AV_CODEC_ID_AV1 && av1_depth == 10)) &&
     video_settings.video_accel_p010_playback) {
    add_source_color_extensions(config_dict, mcp);
    TRACE(TRACE_INFO, "VTB",
          "%s P010 format description preserves source color metadata: transfer=%d, primaries=%d, matrix=%d",
          vtb_codec_name(mc->codec_id),
          mcp->color_transfer, mcp->color_primaries, mcp->color_matrix);
  }
#endif

  // Setup extradata when the container provides a codec configuration atom.
  if(config_atom != NULL && codec_config != NULL && codec_config_size != 0) {
    CFMutableDictionaryRef extradata_dict =
      CFDictionaryCreateMutable(kCFAllocatorDefault,
                                1,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);

    CFDataRef extradata = CFDataCreate(kCFAllocatorDefault, codec_config,
                                       codec_config_size);
    CFDictionarySetValue(extradata_dict, config_atom, extradata);
    CFRelease(extradata);
    CFDictionarySetValue(config_dict,
                         kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
                         extradata_dict);
    CFRelease(extradata_dict);
  }
  free(owned_codec_config);

  // Enable and force HW accelration
  CFDictionarySetValue(config_dict,
                       kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                       kCFBooleanTrue);

  CFDictionarySetValue(config_dict,
                       kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
                       kCFBooleanTrue);

  CMVideoFormatDescriptionRef fmt;

  status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                          codec_type,
                                          mcp->width,
                                          mcp->height,
                                          config_dict,
                                          &fmt);
  if(status) {
    TRACE(TRACE_DEBUG, "VTB", "Unable to create description %d", status);
    return 1;
  }

  if(((mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2) ||
      (mc->codec_id == AV_CODEC_ID_AV1 && av1_depth == 10)) &&
     video_settings.video_accel_probe_p010)
    probe_p010_output(fmt, config_dict, mcp->width, mcp->height);


  CFMutableDictionaryRef surface_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault,
                              4,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(surface_dict,
#if !TARGET_OS_IPHONE
                       kCVPixelBufferOpenGLCompatibilityKey,
#else
                       kCVPixelBufferOpenGLESCompatibilityKey,
#endif
                       kCFBooleanTrue);

#if TARGET_OS_IPHONE
  /* Ensure VideoToolbox returns IOSurface-backed pixel buffers.  The iOS GLW
   * renderer imports these through CVOpenGLESTextureCache, avoiding a CPU copy
   * of both NV12 planes on older devices such as the A9 iPhone SE. */
  CFMutableDictionaryRef iosurface_dict =
    CFDictionaryCreateMutable(kCFAllocatorDefault,
                              0,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(surface_dict,
                       kCVPixelBufferIOSurfacePropertiesKey,
                       iosurface_dict);
  CFRelease(iosurface_dict);
#endif

  vtb_decoder_t *vtbd = calloc(1, sizeof(vtb_decoder_t));
  vtbd->vtbd_codec_id = mc->codec_id;
  vtbd->vtbd_profile = stream_profile;
  vtbd->vtbd_color_transfer = mcp->color_transfer;
  vtbd->vtbd_assumed_sdr = assumed_sdr;
  vtbd->vtbd_dolby_vision_tag =
    mcp->codec_tag == MKTAG('d', 'v', 'h', 'e') ||
    mcp->codec_tag == MKTAG('d', 'v', 'h', '1');
  vtbd->vtbd_hdr_to_sdr =
                         ((mc->codec_id == AV_CODEC_ID_HEVC &&
                           stream_profile == 2) ||
                          (mc->codec_id == AV_CODEC_ID_AV1 &&
                           av1_depth == 10)) &&
                          hevc_main10_is_hdr(mcp);
#if TARGET_OS_OSX || TARGET_OS_IPHONE
  vtbd->vtbd_p010_playback = ((mc->codec_id == AV_CODEC_ID_HEVC &&
                              stream_profile == 2) ||
                             (mc->codec_id == AV_CODEC_ID_AV1 &&
                              av1_depth == 10)) &&
                             video_settings.video_accel_p010_playback;
  vtbd->vtbd_p010_direct = vtbd->vtbd_p010_playback &&
                           video_settings.video_accel_p010_direct;
#endif

  const int source_depth = mcp->bits_per_component > 0 ?
    mcp->bits_per_component :
    (mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2 ? 10 :
     mc->codec_id == AV_CODEC_ID_AV1 && av1_depth ? av1_depth : 8);

  dict_set_int32(surface_dict, kCVPixelBufferWidthKey, mcp->width);
  dict_set_int32(surface_dict, kCVPixelBufferHeightKey, mcp->height);

#if TARGET_OS_IPHONE
  vtbd->vtbd_decode_pixel_format = vtbd->vtbd_p010_playback ?
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
    (vtbd->vtbd_hdr_to_sdr ?
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange :
      kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
  vtbd->vtbd_pixel_format = vtbd->vtbd_p010_direct ?
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
    (vtbd->vtbd_p010_playback ?
      kCVPixelFormatType_420YpCbCr8BiPlanarFullRange :
      vtbd->vtbd_decode_pixel_format);
#else
  vtbd->vtbd_decode_pixel_format = vtbd->vtbd_p010_playback ?
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
    kCVPixelFormatType_420YpCbCr8Planar;
  vtbd->vtbd_pixel_format = vtbd->vtbd_p010_direct ?
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
    kCVPixelFormatType_420YpCbCr8Planar;
#endif

  dict_set_int32(surface_dict, kCVPixelBufferPixelFormatTypeKey,
                 vtbd->vtbd_decode_pixel_format);

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  if(vtbd->vtbd_p010_playback && !vtbd->vtbd_p010_direct) {
    CFMutableDictionaryRef iosurface_dict =
      CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(surface_dict, kCVPixelBufferIOSurfacePropertiesKey,
                         iosurface_dict);
    CFRelease(iosurface_dict);
  }
#endif

  int linewidth = mcp->width;

  switch(vtbd->vtbd_decode_pixel_format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
      linewidth *= 2;
      break;
  }

  dict_set_int32(surface_dict, kCVPixelBufferBytesPerRowAlignmentKey, linewidth);

  VTDecompressionOutputCallbackRecord cb = {
    .decompressionOutputCallback = picture_out,
    .decompressionOutputRefCon = vtbd
  };

  /* create decompression session */
  status = VTDecompressionSessionCreate(kCFAllocatorDefault,
                                        fmt,
                                        config_dict,
                                        surface_dict,
                                        &cb,
                                        &vtbd->vtbd_session);

  CFRelease(config_dict);
  CFRelease(surface_dict);

  if(status) {
    TRACE(TRACE_DEBUG, "VTB", "Failed to open -- %d", status);
    CFRelease(fmt);
    return 1;

  }

  if(vtbd->vtbd_hdr_to_sdr && !vtbd->vtbd_p010_playback) {
    /* Apple recommends performing HDR-to-SDR color conversion before, or at
     * the same time as, 10-to-8-bit conversion.  The decompression session's
     * pixel-transfer stage does both and returns BT.709 NV12/YUV420 frames for
     * Movian's existing SDR OpenGL/OpenGLES renderer. */
    CFMutableDictionaryRef transfer_dict =
      CFDictionaryCreateMutable(kCFAllocatorDefault,
                                3,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(transfer_dict,
                         kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                         kCVImageBufferColorPrimaries_ITU_R_709_2);
    CFDictionarySetValue(transfer_dict,
                         kVTPixelTransferPropertyKey_DestinationTransferFunction,
                         kCVImageBufferTransferFunction_ITU_R_709_2);
    CFDictionarySetValue(transfer_dict,
                         kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                         kCVImageBufferYCbCrMatrix_ITU_R_709_2);

    status = VTSessionSetProperty(vtbd->vtbd_session,
                                  kVTDecompressionPropertyKey_PixelTransferProperties,
                                  transfer_dict);
    CFRelease(transfer_dict);

    if(status) {
      TRACE(TRACE_INFO, "VTB",
            "HDR-to-SDR pixel transfer unavailable (status=%d); using software fallback",
            (int)status);
      VTDecompressionSessionInvalidate(vtbd->vtbd_session);
      CFRelease(vtbd->vtbd_session);
      CFRelease(fmt);
      free(vtbd);
      return 1;
    }
  }

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  if(vtbd->vtbd_p010_playback) {
    status = create_p010_sdr_bridge(vtbd, mcp->width, mcp->height);
    if(status) {
      TRACE(TRACE_INFO, "VTB",
            "P010 SDR bridge unavailable (status=%d); using software fallback",
            (int)status);
      VTDecompressionSessionInvalidate(vtbd->vtbd_session);
      CFRelease(vtbd->vtbd_session);
      if(vtbd->vtbd_pixel_transfer != NULL) {
        VTPixelTransferSessionInvalidate(vtbd->vtbd_pixel_transfer);
        CFRelease(vtbd->vtbd_pixel_transfer);
      }
      if(vtbd->vtbd_sdr_pool != NULL)
        CFRelease(vtbd->vtbd_sdr_pool);
      CFRelease(fmt);
      free(vtbd);
      return 1;
    }
  }
#endif
  vtbd->vtbd_fmt = fmt;
  vtbd->vtbd_max_ts   = PTS_UNSET;
  vtbd->vtbd_flush_to = PTS_UNSET;
  vtbd->vtbd_last_pts = PTS_UNSET;

  mc->opaque = vtbd;
  mc->decode = vtb_decode;
  mc->close = vtb_close;
  mc->flush = vtb_flush;

  TRACE(TRACE_INFO, "VTB",
        "Opened %s decoder %dx%d, source-depth=%d, transfer=%d, primaries=%d, matrix=%d, range=%d, hardware=required, pixel-format=%s, renderer=%s",
        mc->codec_id == AV_CODEC_ID_HEVC ?
          (vtbd->vtbd_p010_direct ? "HEVC Main10 direct-P010" :
           vtbd->vtbd_p010_playback ? "HEVC Main10 P010-to-SDR" :
           vtbd->vtbd_hdr_to_sdr ? "HEVC Main10 HDR-to-SDR" :
           vtbd->vtbd_assumed_sdr ? "HEVC Main10 SDR assumed" :
           stream_profile == 2 ? "HEVC Main10 SDR" : "HEVC Main") :
        mc->codec_id == AV_CODEC_ID_AV1 && av1_depth == 10 ?
          (vtbd->vtbd_p010_direct ? "AV1 Main10 direct-P010" :
           vtbd->vtbd_p010_playback ? "AV1 Main10 P010-to-SDR" :
                                      "AV1 Main10") :
        vtb_codec_name(mc->codec_id),
        mcp->width, mcp->height,
        source_depth, mcp->color_transfer,
        mcp->color_primaries, mcp->color_matrix, mcp->color_range,
        vtbd->vtbd_decode_pixel_format ==
          kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? "NV12 full-range" :
        vtbd->vtbd_decode_pixel_format ==
          kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ? "NV12 video-range" :
        vtbd->vtbd_decode_pixel_format ==
          kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? "P010 video-range" :
                                                           "planar 4:2:0",
#if TARGET_OS_IPHONE
        vtbd->vtbd_p010_direct ? "OpenGL ES byte-packed P010" :
                                 "IOSurface zero-copy"
#else
        vtbd->vtbd_p010_direct ? "OpenGL IOSurface zero-copy" : "OpenGL"
#endif
        );
  return 0;
}

REGISTER_CODEC(NULL, video_vtb_codec_create, 10);
