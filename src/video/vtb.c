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
#include "notifications.h"

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
  CFDictionaryRef vtbd_decoder_spec;
  CFDictionaryRef vtbd_surface_attrs;
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
  int vtbd_nal_length_size;
  int vtbd_profile;
  int vtbd_color_transfer;
  float vtbd_hdr_peak_luminance;
  float vtbd_sei_mastering_peak_luminance;
  unsigned vtbd_sei_max_cll;
  int vtbd_hdr_to_sdr;
  int vtbd_assumed_sdr;
  int vtbd_p010_playback;
  int vtbd_p010_direct;
  int vtbd_nv12_hdr_direct;
  int vtbd_dolby_vision_tag;
  int vtbd_dolby_vision_profile;
  int vtbd_hw_status_reported;
  int vtbd_output_reported;
  int vtbd_sdr_output_reported;
  unsigned int vtbd_decode_errors;
  int vtbd_decode_error_notified;
  int vtbd_hdr10plus_reported;
  int vtbd_dv5_transfer_assumed_reported;
} vtb_decoder_t;

static uint16_t
read_be16(const uint8_t *p)
{
  return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t
read_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static CFDataRef
vtb_parse_hevc_hdr_sei(vtb_decoder_t *vtbd, const uint8_t *data, size_t size)
{
  CFDataRef hdr10plus = NULL;
  const int nls = vtbd->vtbd_nal_length_size;
  if(nls < 1 || nls > 4)
    return NULL;

  while(size >= (size_t)nls) {
    uint32_t nal_size = 0;
    for(int i = 0; i < nls; i++)
      nal_size = (nal_size << 8) | data[i];
    data += nls;
    size -= nls;
    if(nal_size > size || nal_size < 3)
      break;

    const int nal_type = (data[0] >> 1) & 0x3f;
    if(nal_type == 39 || nal_type == 40) {
      uint8_t *rbsp = malloc(nal_size - 2);
      size_t rbsp_size = 0;
      int zeros = 0;
      for(uint32_t i = 2; i < nal_size; i++) {
        if(zeros >= 2 && data[i] == 3) {
          zeros = 0;
          continue;
        }
        rbsp[rbsp_size++] = data[i];
        zeros = data[i] == 0 ? zeros + 1 : 0;
      }

      size_t off = 0;
      while(off + 2 <= rbsp_size && rbsp[off] != 0x80) {
        unsigned payload_type = 0;
        while(off < rbsp_size && rbsp[off] == 0xff) {
          payload_type += 255;
          off++;
        }
        if(off >= rbsp_size)
          break;
        payload_type += rbsp[off++];

        size_t payload_size = 0;
        while(off < rbsp_size && rbsp[off] == 0xff) {
          payload_size += 255;
          off++;
        }
        if(off >= rbsp_size)
          break;
        payload_size += rbsp[off++];
        if(payload_size > rbsp_size - off)
          break;

        if(payload_type == 137 && payload_size >= 24) {
          const float mastering_peak =
            read_be32(rbsp + off + 16) / 10000.0f;
          if(mastering_peak >= 100 && mastering_peak <= 10000 &&
             fabsf(vtbd->vtbd_sei_mastering_peak_luminance -
                   mastering_peak) >= 1.0f) {
            vtbd->vtbd_sei_mastering_peak_luminance = mastering_peak;
          }
        } else if(payload_type == 144 && payload_size >= 4) {
          const unsigned max_cll = read_be16(rbsp + off);
          if(max_cll >= 100 && max_cll <= 10000 &&
             vtbd->vtbd_sei_max_cll != max_cll) {
            vtbd->vtbd_sei_max_cll = max_cll;
          }
        } else if(payload_type == 4 && payload_size >= 7 &&
                  rbsp[off] == 0xb5 &&
                  read_be16(rbsp + off + 1) == 0x003c &&
                  read_be16(rbsp + off + 3) == 0x0001 &&
                  rbsp[off + 5] == 4) {
          /* SMPTE ST 2094-40 uses application_identifier 4 inside a
           * registered ITU-T T.35 SEI message.  CoreMedia expects the exact
           * T.35 body beginning with country_code, not the HEVC SEI header or
           * emulation-prevention bytes. */
          if(hdr10plus != NULL)
            CFRelease(hdr10plus);
          hdr10plus = CFDataCreate(kCFAllocatorDefault, rbsp + off,
                                   payload_size);
        }

        const float peak = vtbd->vtbd_sei_max_cll != 0 ?
          vtbd->vtbd_sei_max_cll : vtbd->vtbd_sei_mastering_peak_luminance;
        if(peak > 0)
          vtbd->vtbd_hdr_peak_luminance = peak;
        off += payload_size;
      }
      free(rbsp);
    }
    data += nal_size;
    size -= nal_size;
  }
  return hdr10plus;
}

static void
vtb_update_hdr_peak_from_pixel_buffer(vtb_decoder_t *vtbd,
                                      CVPixelBufferRef pixel_buffer)
{
  unsigned max_cll = 0;
  float mastering_peak = 0;
  CFTypeRef light = CVBufferGetAttachment(
    pixel_buffer, kCVImageBufferContentLightLevelInfoKey, NULL);
  if(light != NULL && CFGetTypeID(light) == CFDataGetTypeID() &&
     CFDataGetLength((CFDataRef)light) >= 4) {
    const uint8_t *p = CFDataGetBytePtr((CFDataRef)light);
    max_cll = read_be16(p);
  }

  CFTypeRef mastering = CVBufferGetAttachment(
    pixel_buffer, kCVImageBufferMasteringDisplayColorVolumeKey, NULL);
  if(mastering != NULL && CFGetTypeID(mastering) == CFDataGetTypeID() &&
     CFDataGetLength((CFDataRef)mastering) >= 24) {
    const uint8_t *p = CFDataGetBytePtr((CFDataRef)mastering);
    mastering_peak = read_be32(p + 16) / 10000.0f;
  }

  float peak = max_cll >= 100 && max_cll <= 10000 ? max_cll :
               mastering_peak >= 100 && mastering_peak <= 10000 ?
                 mastering_peak : vtbd->vtbd_hdr_peak_luminance;
  if(peak < 100)
    peak = 1000;
  vtbd->vtbd_hdr_peak_luminance = peak;
}

static void dict_set_int32(CFMutableDictionaryRef dict, CFStringRef key,
                           int value);
static void picture_out(void *decompressionOutputRefCon,
                        void *sourceFrameRefCon,
                        OSStatus status,
                        VTDecodeInfoFlags infoFlags,
                        CVPixelBufferRef imageBuffer,
                        CMTime pts,
                        CMTime duration);

static OSStatus
vtb_create_session(vtb_decoder_t *vtbd)
{
  VTDecompressionOutputCallbackRecord cb = {
    .decompressionOutputCallback = picture_out,
    .decompressionOutputRefCon = vtbd
  };

  OSStatus status =
    VTDecompressionSessionCreate(kCFAllocatorDefault,
                                 vtbd->vtbd_fmt,
                                 vtbd->vtbd_decoder_spec,
                                 vtbd->vtbd_surface_attrs,
                                 &cb,
                                 &vtbd->vtbd_session);
  if(status == noErr && vtbd->vtbd_session != NULL &&
     vtbd->vtbd_color_transfer == AVCOL_TRC_SMPTE2084) {
    OSStatus metadata_status = VTSessionSetProperty(
      vtbd->vtbd_session,
      kVTDecompressionPropertyKey_PropagatePerFrameHDRDisplayMetadata,
      kCFBooleanTrue);
#if TARGET_OS_IPHONE
    static int unsupported_reported;
    if(metadata_status == kVTPropertyNotSupportedErr) {
      if(__sync_bool_compare_and_swap(&unsupported_reported, 0, 1))
        TRACE(TRACE_INFO, "VTB",
              "Per-frame HDR metadata propagation is not supported by this iOS VideoToolbox implementation (status=%d); continuing with pixel-buffer metadata",
              (int)metadata_status);
    } else
#endif
    {
      TRACE(metadata_status == noErr ? TRACE_INFO : TRACE_ERROR, "VTB",
            "Per-frame HDR metadata propagation %s (status=%d)",
            metadata_status == noErr ? "enabled" : "failed",
            (int)metadata_status);
    }
  }
  if(status || !vtbd->vtbd_hdr_to_sdr || vtbd->vtbd_p010_playback)
    return status;

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
    VTDecompressionSessionInvalidate(vtbd->vtbd_session);
    CFRelease(vtbd->vtbd_session);
    vtbd->vtbd_session = NULL;
  }
  return status;
}

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
  case AV_CODEC_ID_PRORES:
    return "ProRes";
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
 * Dolby Vision profile 5 commonly has unspecified conventional HEVC colour
 * fields because its source signal is described by Dolby metadata.  Apple's
 * Dolby-aware decoder attaches the transfer function of its actual output.
 * Use that value for renderer selection instead of retaining the unspecified
 * container value and incorrectly drawing PQ output through the SDR shader.
 */
static int
output_color_transfer(CVPixelBufferRef imageBuffer, int fallback,
                      int *metadata_found)
{
  CFTypeRef value = CVBufferCopyAttachment(
    imageBuffer, kCVImageBufferTransferFunctionKey, NULL);
  int transfer = fallback;
  *metadata_found = 0;
  if(value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
    *metadata_found = 1;
    if(CFEqual(value, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ))
      transfer = AVCOL_TRC_SMPTE2084;
    else if(CFEqual(value, kCVImageBufferTransferFunction_ITU_R_2100_HLG))
      transfer = AVCOL_TRC_ARIB_STD_B67;
    else if(CFEqual(value, kCVImageBufferTransferFunction_ITU_R_709_2))
      transfer = AVCOL_TRC_BT709;
  }
  if(value != NULL)
    CFRelease(value);
  return transfer;
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

  if(vtbd->vtbd_color_transfer == AVCOL_TRC_SMPTE2084)
    vtb_update_hdr_peak_from_pixel_buffer(vtbd, vf->vf_buf);

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
  fi.fi_hdr_peak_luminance = vtbd->vtbd_hdr_peak_luminance;
  fi.fi_epoch = vf->vf_mbm.mbm_epoch;
  fi.fi_drive_clock = vf->vf_mbm.mbm_drive_clock;
  fi.fi_user_time = vf->vf_mbm.mbm_user_time;
  fi.fi_vshift = 1;
  fi.fi_hshift = 1;
  /* Never allow malformed container/parser timing to pin one decoded frame
   * on screen for seconds or hours.  Durations outside the useful video-frame
   * range are replaced by the PTS-derived estimate. */
  fi.fi_duration = vf->vf_mbm.mbm_duration > 1000 &&
                   vf->vf_mbm.mbm_duration < 1000000 ?
                   vf->vf_mbm.mbm_duration : vtbd->vtbd_estimated_duration;

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
      fi.fi_type = vtbd->vtbd_nv12_hdr_direct ? 'NVHE' : 'CVPB';
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
  /* VideoToolbox callbacks are asynchronous.  sourceFrameRefCon therefore
   * owns a per-submission metadata copy; release it as soon as the callback
   * has taken a stack copy.  The old code pointed into vd_reorder[], whose
   * slots could wrap and be overwritten during rapid seek pre-roll. */
  media_buf_meta_t mbm_storage = *(media_buf_meta_t *)sourceFrameRefCon;
  free(sourceFrameRefCon);
  media_buf_meta_t *mbm = &mbm_storage;
  vtb_decoder_t *vtbd = decompressionOutputRefCon;

  if(status != noErr) {
    vtbd->vtbd_decode_errors++;
    if(vtbd->vtbd_decode_errors == 1 ||
       !(vtbd->vtbd_decode_errors % 60))
      TRACE(TRACE_ERROR, "VTB",
            "Asynchronous decode failed: status=%d flags=0x%x frame=%u%s",
            (int)status, (unsigned int)infoFlags,
            vtbd->vtbd_decode_errors,
            vtbd->vtbd_dolby_vision_tag ? " Dolby-Vision" : "");
    if(vtbd->vtbd_dolby_vision_profile == 5 &&
       status == kVTVideoDecoderMalfunctionErr &&
       !vtbd->vtbd_decode_error_notified) {
      vtbd->vtbd_decode_error_notified = 1;
      notify_add(NULL, NOTIFY_ERROR, NULL, 8,
                 _("Dolby Vision Profile 5 playback is not supported on this Apple device"));
      if(vtbd->vtbd_vd != NULL && vtbd->vtbd_vd->vd_mp != NULL)
        prop_set(vtbd->vtbd_vd->vd_mp->mp_prop_root,
                 "loading", PROP_SET_INT, 0);
    }
  }

  if(imageBuffer == NULL)
    return; // No frame, typically from kVTDecodeFrame_DoNotOutputFrame

  /* Pre-roll frames must still be decoded so the hardware session builds the
   * reference-picture state needed after a random-access point.  Suppress
   * them here instead of passing kVTDecodeFrame_DoNotOutputFrame, which can
   * leave VideoToolbox waiting for another sync sample after a forward seek. */
  if(mbm->mbm_skip)
    return;

  if(!vtbd->vtbd_output_reported) {
    report_output_format(vtbd, imageBuffer, "Decoder");
    vtbd->vtbd_output_reported = 1;
  }

  if(vtbd->vtbd_dolby_vision_tag) {
    int metadata_found;
    int output_transfer = output_color_transfer(
      imageBuffer, vtbd->vtbd_color_transfer, &metadata_found);
    if(!metadata_found && vtbd->vtbd_dolby_vision_profile == 5) {
      output_transfer = AVCOL_TRC_SMPTE2084;
      if(!vtbd->vtbd_dv5_transfer_assumed_reported) {
        vtbd->vtbd_dv5_transfer_assumed_reported = 1;
        TRACE(TRACE_INFO, "VTB",
              "Dolby Vision Profile 5 decoder output has no transfer attachment; using its defined PQ transfer for rendering");
      }
    }
    if(output_transfer != vtbd->vtbd_color_transfer) {
      TRACE(TRACE_INFO, "VTB",
            "Dolby Vision renderer transfer updated from %d to %d using decoder output metadata",
            vtbd->vtbd_color_transfer, output_transfer);
      vtbd->vtbd_color_transfer = output_transfer;
      if(output_transfer == AVCOL_TRC_SMPTE2084 ||
         output_transfer == AVCOL_TRC_ARIB_STD_B67)
        vtbd->vtbd_hdr_to_sdr = 1;
    }
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

  CFDataRef hdr10plus = NULL;
  if(vtbd->vtbd_codec_id == AV_CODEC_ID_HEVC)
    hdr10plus = vtb_parse_hevc_hdr_sei(vtbd, mb->mb_data, mb->mb_size);

  if(vtbd->vtbd_session == NULL) {
    if(hdr10plus != NULL)
      CFRelease(hdr10plus);
    TRACE(TRACE_ERROR, "VTB", "Decoder session is unavailable");
    return;
  }

  status =
    CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                       mb->mb_data, mb->mb_size,
                                       kCFAllocatorNull,
                                       NULL, 0, mb->mb_size, 0, &block_buf);
  if(status) {
    if(hdr10plus != NULL)
      CFRelease(hdr10plus);
    TRACE(TRACE_ERROR, "VTB", "Data buffer allocation error %d", status);
    return;
  }

  CMSampleTimingInfo ti;

  ti.duration = mb->mb_duration > 0 && mb->mb_duration < 1000000 ?
                CMTimeMake(mb->mb_duration, 1000000) : kCMTimeInvalid;
  ti.presentationTimeStamp = CMTimeMake(mb->mb_pts, 1000000);
  ti.decodeTimeStamp       = CMTimeMake(mb->mb_dts, 1000000);

  status =
    CMSampleBufferCreate(kCFAllocatorDefault,
                         block_buf, TRUE, 0, 0, vtbd->vtbd_fmt,
                         1, 1, &ti, 0, NULL, &sample_buf);

  CFRelease(block_buf);
  if(status) {
    if(hdr10plus != NULL)
      CFRelease(hdr10plus);
    TRACE(TRACE_ERROR, "VTB", "Sample buffer allocation error %d", status);
    return;
  }

  /* CMSampleBufferCreate() does not infer random-access information from the
   * compressed payload.  Tell VideoToolbox explicitly whether this packet is
   * a sync sample.  Playback from the head of a stream can work without these
   * attachments, while decoding after an av_seek_frame() may wait forever for
   * a keyframe that the session does not recognize. */
  CFArrayRef attachments =
    CMSampleBufferGetSampleAttachmentsArray(sample_buf, TRUE);
  if(attachments != NULL && CFArrayGetCount(attachments) != 0) {
    CFMutableDictionaryRef attachment =
      (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    if(mb->mb_keyframe) {
      CFDictionaryRemoveValue(attachment, kCMSampleAttachmentKey_NotSync);
      CFDictionaryRemoveValue(attachment,
                              kCMSampleAttachmentKey_DependsOnOthers);
    } else {
      CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync,
                           kCFBooleanTrue);
      CFDictionarySetValue(attachment,
                           kCMSampleAttachmentKey_DependsOnOthers,
                           kCFBooleanTrue);
    }
#if TARGET_OS_OSX || TARGET_OS_IPHONE
    if(hdr10plus != NULL) {
      if(__builtin_available(macOS 13.0, iOS 16.0, tvOS 16.0, *)) {
        CFDictionarySetValue(attachment,
                             kCMSampleAttachmentKey_HDR10PlusPerFrameData,
                             hdr10plus);
        if(!vtbd->vtbd_hdr10plus_reported) {
          vtbd->vtbd_hdr10plus_reported = 1;
          TRACE(TRACE_INFO, "VTB",
                "HDR10+ ST 2094-40 metadata attached to VideoToolbox samples");
        }
      }
    }
#endif
  }

  if(hdr10plus != NULL)
    CFRelease(hdr10plus);

  media_buf_meta_t *frame_opaque = malloc(sizeof(*frame_opaque));
  if(frame_opaque == NULL) {
    CFRelease(sample_buf);
    TRACE(TRACE_ERROR, "VTB", "Frame metadata allocation failed");
    return;
  }
  copy_mbm_from_mb(frame_opaque, mb);

  status =
    VTDecompressionSessionDecodeFrame(vtbd->vtbd_session, sample_buf, flags,
                                      frame_opaque, &infoflags);
  CFRelease(sample_buf);
  if(status) {
    free(frame_opaque);
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
  VTDecompressionSessionInvalidate(vtbd->vtbd_session);
  CFRelease(vtbd->vtbd_session);
  vtbd->vtbd_session = NULL;
  hts_mutex_lock(&vtbd->vtbd_mutex);
  destroy_frames(vtbd);
  vtbd->vtbd_max_ts   = PTS_UNSET;
  vtbd->vtbd_flush_to = PTS_UNSET;
  vtbd->vtbd_last_pts = PTS_UNSET;
  vtbd->vtbd_output_reported = 0;
  vtbd->vtbd_hw_status_reported = 0;
  vtbd->vtbd_sdr_output_reported = 0;
  vtbd->vtbd_decode_errors = 0;
  vtbd->vtbd_decode_error_notified = 0;
  hts_mutex_unlock(&vtbd->vtbd_mutex);

  OSStatus status = vtb_create_session(vtbd);
  if(status) {
    TRACE(TRACE_ERROR, "VTB",
          "Unable to recreate decoder after seek (status=%d)", (int)status);
  }
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

  CFRelease(vtbd->vtbd_decoder_spec);
  CFRelease(vtbd->vtbd_surface_attrs);

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


/* Some dvhe/hev1 MP4 files carry a minimal 23-byte hvcC record and repeat
 * VPS/SPS/PPS in-band at each random-access point.  VideoToolbox cannot create
 * a session from the empty hvcC record, while libavcodec can and consequently
 * becomes an incorrect 8-bit fallback for HDR.  Delay decoder creation until
 * the first access unit and complete hvcC from its parameter-set NAL units. */
typedef struct hevc_inband_config {
  media_codec_t *decoder;
  media_codec_params_t params;
  uint8_t *base_hvcc;
  uint8_t *hvcc;
} hevc_inband_config_t;

static int
hevc_complete_hvcc(const media_codec_params_t *mcp,
                   const uint8_t *sample, size_t sample_size,
                   uint8_t **result, size_t *result_size)
{
  const uint8_t *base = mcp->extradata;
  const int nls = (base[21] & 3) + 1;
  const uint8_t *ps[3] = {NULL, NULL, NULL};
  size_t ps_size[3] = {0, 0, 0};

  while(sample_size >= (size_t)nls) {
    uint32_t nal_size = 0;
    for(int i = 0; i < nls; i++)
      nal_size = (nal_size << 8) | sample[i];
    sample += nls;
    sample_size -= nls;
    if(nal_size > sample_size || nal_size < 2)
      break;

    const int nal_type = (sample[0] >> 1) & 0x3f;
    if(nal_type >= 32 && nal_type <= 34 && ps[nal_type - 32] == NULL) {
      ps[nal_type - 32] = sample;
      ps_size[nal_type - 32] = nal_size;
    }
    sample += nal_size;
    sample_size -= nal_size;
  }

  if(ps[0] == NULL || ps[1] == NULL || ps[2] == NULL)
    return -1;

  size_t size = 23;
  for(int i = 0; i < 3; i++) {
    /* hvcC stores each NAL-unit length in 16 bits. Reject a malformed access
     * unit instead of truncating that field while copying the full payload. */
    if(ps_size[i] > UINT16_MAX)
      return -1;
    size += 5 + ps_size[i];
  }
  uint8_t *hvcc = malloc(size);
  if(hvcc == NULL)
    return -1;

  memcpy(hvcc, base, 23);
  hvcc[22] = 3;
  uint8_t *p = hvcc + 23;
  for(int i = 0; i < 3; i++) {
    *p++ = 0x80 | (32 + i); /* array_complete + NAL unit type */
    *p++ = 0;
    *p++ = 1;
    *p++ = ps_size[i] >> 8;
    *p++ = ps_size[i];
    memcpy(p, ps[i], ps_size[i]);
    p += ps_size[i];
  }
  *result = hvcc;
  *result_size = size;
  return 0;
}

static void
hevc_inband_decode(media_codec_t *mc, video_decoder_t *vd,
                   media_queue_t *mq, media_buf_t *mb, int reqsize)
{
  hevc_inband_config_t *hic = mc->opaque;
  if(hic->decoder == NULL) {
    size_t hvcc_size;
    if(hevc_complete_hvcc(&hic->params, mb->mb_data, mb->mb_size,
                          &hic->hvcc, &hvcc_size)) {
      return;
    }
    hic->params.extradata = hic->hvcc;
    hic->params.extradata_size = hvcc_size;
    hic->decoder = media_codec_create(mc->codec_id, 0, NULL, NULL,
                                      &hic->params, mc->mp);
    if(hic->decoder == NULL) {
      /* A rejected nested decoder is retryable at a later random-access
       * point, but the completed configuration must not leak on every frame. */
      free(hic->hvcc);
      hic->hvcc = NULL;
      hic->params.extradata = hic->base_hvcc;
      hic->params.extradata_size = 23;
      return;
    }
    TRACE(TRACE_INFO, "VTB",
          "Bootstrapped HEVC decoder from in-band VPS/SPS/PPS (%zu-byte hvcC)",
          hvcc_size);
  }
  hic->decoder->decode(hic->decoder, vd, mq, mb, reqsize);
}

static void
hevc_inband_flush(media_codec_t *mc, video_decoder_t *vd)
{
  hevc_inband_config_t *hic = mc->opaque;
  if(hic->decoder != NULL && hic->decoder->flush != NULL)
    hic->decoder->flush(hic->decoder, vd);
}

static void
hevc_inband_close(media_codec_t *mc)
{
  hevc_inband_config_t *hic = mc->opaque;
  if(hic->decoder != NULL)
    media_codec_deref(hic->decoder);
  free(hic->base_hvcc);
  free(hic->hvcc);
  free(hic);
}

static int
hevc_inband_open(media_codec_t *mc, const media_codec_params_t *mcp)
{
  hevc_inband_config_t *hic = calloc(1, sizeof(*hic));
  if(hic == NULL)
    return 1;
  hic->params = *mcp;
  uint8_t *base = malloc(mcp->extradata_size);
  if(base == NULL) {
    free(hic);
    return 1;
  }
  memcpy(base, mcp->extradata, mcp->extradata_size);
  hic->base_hvcc = base;
  hic->params.extradata = base;
  mc->opaque = hic;
  mc->decode = hevc_inband_decode;
  mc->flush = hevc_inband_flush;
  mc->close = hevc_inband_close;
  TRACE(TRACE_INFO, "VTB",
        "Deferring HEVC decoder creation for in-band VPS/SPS/PPS");
  return 0;
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

  if(mcp == NULL)
    return 1;

  const int dolby_vision = mcp->dovi_valid ||
    mcp->codec_tag == MKTAG('d', 'v', 'h', 'e') ||
    mcp->codec_tag == MKTAG('d', 'v', 'h', '1');
  const int dolby_profile5 = mcp->dovi_valid && mcp->dovi_profile == 5;

  switch(mc->codec_id) {
  case AV_CODEC_ID_H264:
    codec_type = kCMVideoCodecType_H264;
    config_atom = CFSTR("avcC");
    break;
  case AV_CODEC_ID_HEVC:
    /* Profile 5 has no HDR10-compatible base-layer representation.  Let
     * Apple's Dolby-aware decoder interpret its RPU/IPT-PQ signal rather than
     * opening it as ordinary HEVC and feeding those planes to the generic PQ
     * shader.  Keep compatible Dolby profiles on the proven HEVC path. */
    codec_type = dolby_profile5 ?
      kCMVideoCodecType_DolbyVisionHEVC : kCMVideoCodecType_HEVC;
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
#if TARGET_OS_OSX
  case AV_CODEC_ID_PRORES:
    /* FFmpeg exposes the QuickTime ProRes sample-entry tag in host byte
     * order.  Map it to CoreMedia's codec constants rather than passing the
     * numeric tag through directly.  If a demuxer omitted the tag, the
     * profile remains a safe fallback. */
    switch(mcp->codec_tag) {
    case MKTAG('a', 'p', 'c', 'o'):
      codec_type = kCMVideoCodecType_AppleProRes422Proxy;
      break;
    case MKTAG('a', 'p', 'c', 's'):
      codec_type = kCMVideoCodecType_AppleProRes422LT;
      break;
    case MKTAG('a', 'p', 'c', 'n'):
      codec_type = kCMVideoCodecType_AppleProRes422;
      break;
    case MKTAG('a', 'p', 'c', 'h'):
      codec_type = kCMVideoCodecType_AppleProRes422HQ;
      break;
    case MKTAG('a', 'p', '4', 'h'):
      codec_type = kCMVideoCodecType_AppleProRes4444;
      break;
    case MKTAG('a', 'p', '4', 'x'):
      codec_type = kCMVideoCodecType_AppleProRes4444XQ;
      break;
    default:
      switch(mcp->profile) {
      case FF_PROFILE_PRORES_PROXY:
        codec_type = kCMVideoCodecType_AppleProRes422Proxy;
        break;
      case FF_PROFILE_PRORES_LT:
        codec_type = kCMVideoCodecType_AppleProRes422LT;
        break;
      case FF_PROFILE_PRORES_STANDARD:
        codec_type = kCMVideoCodecType_AppleProRes422;
        break;
      case FF_PROFILE_PRORES_HQ:
        codec_type = kCMVideoCodecType_AppleProRes422HQ;
        break;
      case FF_PROFILE_PRORES_4444:
        codec_type = kCMVideoCodecType_AppleProRes4444;
        break;
      case FF_PROFILE_PRORES_XQ:
        codec_type = kCMVideoCodecType_AppleProRes4444XQ;
        break;
      default:
        return 1;
      }
    }
    config_atom = NULL;
    break;
#endif
  default:
    return 1;
  }

  if(!VTIsHardwareDecodeSupported(codec_type)) {
    TRACE(TRACE_DEBUG, "VTB", "No hardware decoder for %s%s",
          vtb_codec_name(mc->codec_id),
          codec_type == kCMVideoCodecType_DolbyVisionHEVC ?
            " Dolby Vision profile 5" : "");
    if(codec_type == kCMVideoCodecType_DolbyVisionHEVC) {
      notify_add(NULL, NOTIFY_ERROR, NULL, 8,
                 _("Dolby Vision Profile 5 playback is not supported on this Apple device"));
      prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 0);
    }
    return 1;
  }

  if(codec_type == kCMVideoCodecType_DolbyVisionHEVC)
    TRACE(TRACE_INFO, "VTB",
          "Using Apple Dolby Vision HEVC decoder for profile 5");

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
    } else if(mc->codec_id == AV_CODEC_ID_PRORES) {
      config_atom = NULL;
    } else
    return h264_annexb_to_avc(mc, mp, &video_vtb_codec_create);
  }

  const uint8_t *codec_config = mcp->extradata;
  size_t codec_config_size = mcp->extradata_size;
  uint8_t *owned_codec_config = NULL;
  if(mc->codec_id != AV_CODEC_ID_AV1 && mc->codec_id != AV_CODEC_ID_VP9 &&
     mc->codec_id != AV_CODEC_ID_PRORES &&
     codec_config[0] != 1) {
    if(mc->codec_id == AV_CODEC_ID_HEVC)
      return 1;
    return h264_annexb_to_avc(mc, mp, &video_vtb_codec_create);
  }

  if(mc->codec_id == AV_CODEC_ID_HEVC && codec_config_size == 23 &&
     codec_config[22] == 0)
    return hevc_inband_open(mc, mcp);

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

    if(stream_profile == 2 && !dolby_vision && !hevc_main10_is_sdr(mcp) &&
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
    if(dolby_vision) {
      TRACE(TRACE_INFO, "VTB",
            "Dolby Vision stream detected: profile=%d level=%d RPU=%d EL=%d BL=%d compatibility=%d",
            mcp->dovi_profile, mcp->dovi_level,
            mcp->dovi_rpu_present, mcp->dovi_el_present,
            mcp->dovi_bl_present, mcp->dovi_bl_compatibility_id);
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
  if((dolby_profile5 ||
      (mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2) ||
      (mc->codec_id == AV_CODEC_ID_AV1 && av1_depth == 10)) &&
     video_settings.video_accel_p010_playback) {
    add_source_color_extensions(config_dict, mcp);
    TRACE(TRACE_INFO, "VTB",
          "%s P010 format description preserves source color metadata: transfer=%d, primaries=%d, matrix=%d",
          vtb_codec_name(mc->codec_id),
          mcp->color_transfer, mcp->color_primaries, mcp->color_matrix);
  } else if(mc->codec_id == AV_CODEC_ID_H264 &&
            hevc_main10_is_hdr(mcp)) {
    /* HLG is also used with 8-bit AVC (for example Sony XAVC camera clips).
     * It remains an NV12 decode, but the format description must retain the
     * BT.2020/HLG signal so AVSampleBufferDisplayLayer can present it as EDR
     * instead of the legacy OpenGL renderer treating it as ordinary SDR. */
    add_source_color_extensions(config_dict, mcp);
    TRACE(TRACE_INFO, "VTB",
          "H264 HDR format description preserves source color metadata: transfer=%d, primaries=%d, matrix=%d",
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

    /* FFmpeg exposes the dvcC/dvvC configuration as stream side data rather
     * than folding it into hvcC.  Profile 5 has no compatible base layer, so
     * pass its Dolby configuration to Apple's Dolby-aware decoder.  Profiles
     * 8.1 and 8.4 do have HDR10 and HLG base layers respectively; attaching
     * dvvC makes some iOS versions select a Dolby path that accepts the
     * session but rejects every submitted frame.  Omit it for Profile 8 and
     * let the ordinary HEVC decoder consume the compatible base layer. */
    if(mc->codec_id == AV_CODEC_ID_HEVC && mcp->dovi_valid &&
       mcp->dovi_profile == 5) {
      uint8_t dvcc[24] = {0};
      dvcc[0] = mcp->dovi_version_major;
      dvcc[1] = mcp->dovi_version_minor;
      uint16_t flags = (mcp->dovi_profile << 9) |
                       (mcp->dovi_level << 3) |
                       (mcp->dovi_rpu_present << 2) |
                       (mcp->dovi_el_present << 1) |
                       mcp->dovi_bl_present;
      dvcc[2] = flags >> 8;
      dvcc[3] = flags;
      dvcc[4] = mcp->dovi_bl_compatibility_id << 4;
      CFDataRef dovi_data = CFDataCreate(kCFAllocatorDefault, dvcc,
                                         sizeof(dvcc));
      CFDictionarySetValue(extradata_dict,
                           mcp->dovi_profile > 7 ? CFSTR("dvvC") : CFSTR("dvcC"),
                           dovi_data);
      CFRelease(dovi_data);
    }
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

  if((dolby_profile5 ||
      (mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2) ||
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
  if(mc->codec_id == AV_CODEC_ID_HEVC && codec_config_size > 21)
    vtbd->vtbd_nal_length_size = (codec_config[21] & 3) + 1;
  vtbd->vtbd_profile = stream_profile;
  vtbd->vtbd_color_transfer = mcp->color_transfer;
  vtbd->vtbd_hdr_peak_luminance = mcp->hdr_max_cll != 0 ?
    mcp->hdr_max_cll : mcp->hdr_mastering_max_luminance;
  if(vtbd->vtbd_hdr_peak_luminance < 100.0f)
    vtbd->vtbd_hdr_peak_luminance = 1000.0f;
  vtbd->vtbd_assumed_sdr = assumed_sdr;
  /* This records the decoder path, not merely the source tag.  Profile 8 is
   * deliberately decoded as its HDR10/HLG HEVC base layer and must not enter
   * Dolby-specific output handling or error reporting after a seek. */
  vtbd->vtbd_dolby_vision_tag =
    codec_type == kCMVideoCodecType_DolbyVisionHEVC;
  vtbd->vtbd_dolby_vision_profile = mcp->dovi_valid ?
    mcp->dovi_profile : 0;
  vtbd->vtbd_hdr_to_sdr = dolby_profile5 ||
                         (hevc_main10_is_hdr(mcp) &&
                          ((mc->codec_id == AV_CODEC_ID_HEVC &&
                            stream_profile == 2) ||
                           (mc->codec_id == AV_CODEC_ID_AV1 &&
                            av1_depth == 10)));
#if TARGET_OS_OSX || TARGET_OS_IPHONE
  vtbd->vtbd_p010_playback = (dolby_profile5 ||
                             (mc->codec_id == AV_CODEC_ID_HEVC &&
                              stream_profile == 2) ||
                             (mc->codec_id == AV_CODEC_ID_AV1 &&
                              av1_depth == 10)) &&
                             video_settings.video_accel_p010_playback;
  vtbd->vtbd_p010_direct = vtbd->vtbd_p010_playback &&
                           video_settings.video_accel_p010_direct;
#if TARGET_OS_OSX
  vtbd->vtbd_nv12_hdr_direct = mc->codec_id == AV_CODEC_ID_H264 &&
                               hevc_main10_is_hdr(mcp) &&
                               video_settings.video_accel_p010_direct;
#endif
#endif

  const int source_depth = mcp->bits_per_component > 0 ?
    mcp->bits_per_component :
    (dolby_profile5 ? 10 :
     mc->codec_id == AV_CODEC_ID_HEVC && stream_profile == 2 ? 10 :
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
    (vtbd->vtbd_nv12_hdr_direct ?
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange :
      kCVPixelFormatType_420YpCbCr8Planar);
  vtbd->vtbd_pixel_format = vtbd->vtbd_p010_direct ?
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
    (vtbd->vtbd_nv12_hdr_direct ?
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange :
      kCVPixelFormatType_420YpCbCr8Planar);
#endif

  dict_set_int32(surface_dict, kCVPixelBufferPixelFormatTypeKey,
                 vtbd->vtbd_decode_pixel_format);

#if TARGET_OS_OSX || TARGET_OS_IPHONE
  if((vtbd->vtbd_p010_playback && !vtbd->vtbd_p010_direct) ||
     vtbd->vtbd_nv12_hdr_direct) {
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

  vtbd->vtbd_fmt = fmt;
  vtbd->vtbd_decoder_spec = CFRetain(config_dict);
  vtbd->vtbd_surface_attrs = CFRetain(surface_dict);

  /* create decompression session */
  status = vtb_create_session(vtbd);

  CFRelease(config_dict);
  CFRelease(surface_dict);

  if(status) {
    TRACE(TRACE_DEBUG, "VTB", "Failed to open -- %d", status);
    if(codec_type == kCMVideoCodecType_DolbyVisionHEVC) {
      TRACE(TRACE_INFO, "VTB",
            "Dolby Vision Profile 5 playback rejected by VideoToolbox (status=%d)",
            (int)status);
      notify_add(NULL, NOTIFY_ERROR, NULL, 8,
                 _("Dolby Vision Profile 5 playback is not supported on this Apple device"));
      prop_set(mp->mp_prop_root, "loading", PROP_SET_INT, 0);
    }
    CFRelease(vtbd->vtbd_decoder_spec);
    CFRelease(vtbd->vtbd_surface_attrs);
    CFRelease(fmt);
    free(vtbd);
    return 1;

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
      CFRelease(vtbd->vtbd_decoder_spec);
      CFRelease(vtbd->vtbd_surface_attrs);
      CFRelease(fmt);
      free(vtbd);
      return 1;
    }
  }
#endif
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
          (dolby_profile5 && vtbd->vtbd_p010_direct ?
             "Dolby Vision Profile 5 direct-P010" :
           dolby_profile5 && vtbd->vtbd_p010_playback ?
             "Dolby Vision Profile 5 P010-to-SDR" :
           vtbd->vtbd_p010_direct ? "HEVC Main10 direct-P010" :
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
        vtbd->vtbd_p010_direct ? "Metal P010 bridge / OpenGL ES fallback" :
                                 "IOSurface zero-copy"
#else
        vtbd->vtbd_nv12_hdr_direct ? "Metal NV12 HDR / OpenGL fallback" :
        vtbd->vtbd_p010_direct ? "OpenGL IOSurface zero-copy" : "OpenGL"
#endif
        );
  return 0;
}

REGISTER_CODEC(NULL, video_vtb_codec_create, 10);
