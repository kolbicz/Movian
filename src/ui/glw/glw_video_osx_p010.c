/* Experimental zero-copy P010 IOSurface renderer for macOS. */
#include "main.h"

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#include <assert.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>

#include "libavcodec/avcodec.h"

#include "glw_video_common.h"
#include "arch/osx/osx_c.h"

#define NUM_SURFACES 4

typedef struct p010_aux {
  int import_reported;
  int shader_transfer_reported;
  float reported_headroom;
  int decoder_epoch;
  int decoder_epoch_valid;
  int metal_reported;
  int metal_failed_reported;
} p010_aux_t;

typedef struct reap_task {
  glw_video_reap_task_t hdr;
  GLuint tex[2];
  void *opaque;
} reap_task_t;

static const float cmatrix_ITUR_BT_709[16] = {
  1.164400,  1.164400, 1.164400, 0,
  0.000000, -0.213200, 2.112400, 0,
  1.792700, -0.532900, 0.000000, 0,
 -0.972926,  0.301453,-1.133402, 1
};

static void
do_reap(glw_video_t *gv, reap_task_t *t)
{
  if(t->tex[0] != 0)
    glDeleteTextures(2, t->tex);
  if(t->opaque != NULL)
    CFRelease(t->opaque);
}

static void
surface_reset(glw_video_t *gv, glw_video_surface_t *gvs)
{
  reap_task_t *t = glw_video_add_reap_task(gv, sizeof(reap_task_t), do_reap);
  t->tex[0] = gvs->gvs_texture.textures[0];
  t->tex[1] = gvs->gvs_texture.textures[1];
  t->opaque = gvs->gvs_opaque;
  memset(gvs, 0, sizeof(*gvs));
}

static void
p010_reset(glw_video_t *gv)
{
  for(int i = 0; i < GLW_VIDEO_MAX_SURFACES; i++)
    surface_reset(gv, &gv->gv_surfaces[i]);
  free(gv->gv_aux);
  gv->gv_aux = NULL;
}

static void
surface_init(glw_video_t *gv, glw_video_surface_t *gvs)
{
  TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  hts_cond_signal(&gv->gv_avail_queue_cond);
}

static void
surface_release(glw_video_t *gv, glw_video_surface_t *gvs,
                struct glw_video_surface_queue *fromqueue)
{
  assert(gvs != gv->gv_sa);
  assert(gvs != gv->gv_sb);

  if(gvs->gvs_opaque != NULL) {
    CFRelease(gvs->gvs_opaque);
    gvs->gvs_opaque = NULL;
  }
  gvs->gvs_uploaded = 0;
  TAILQ_REMOVE(fromqueue, gvs, gvs_link);
  TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  hts_cond_signal(&gv->gv_avail_queue_cond);
}

static void
load_texture(glw_root_t *gr, glw_program_t *gp, void *aux,
             const glw_backend_texture_t *b, int num)
{
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, b->textures[1]);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, b->textures[0]);
}

static int
p010_init(glw_video_t *gv)
{
  gv->gv_aux = calloc(1, sizeof(p010_aux_t));
  gv->gv_gpa.gpa_aux = gv;
  gv->gv_gpa.gpa_load_uniforms = glw_video_opengl_load_uniforms;
  gv->gv_gpa.gpa_load_texture = load_texture;
  memcpy(gv->gv_cmatrix_cur, cmatrix_ITUR_BT_709,
         sizeof(gv->gv_cmatrix_cur));
  memcpy(gv->gv_cmatrix_tgt, cmatrix_ITUR_BT_709,
         sizeof(gv->gv_cmatrix_tgt));

  for(int i = 0; i < NUM_SURFACES; i++)
    TAILQ_INSERT_TAIL(&gv->gv_avail_queue, &gv->gv_surfaces[i], gvs_link);

  TRACE(TRACE_INFO, "GLW",
        "Direct P010 IOSurface OpenGL renderer initialized (zero-copy input)");
  return 0;
}

static int64_t
p010_newframe(glw_video_t *gv, video_decoder_t *vd, int flags)
{
  glw_video_surface_t *gvs;
  hts_mutex_assert(&gv->gv_surface_mutex);

  while((gvs = TAILQ_FIRST(&gv->gv_parked_queue)) != NULL) {
    TAILQ_REMOVE(&gv->gv_parked_queue, gvs, gvs_link);
    surface_init(gv, gvs);
  }

  glw_need_refresh(gv->w.glw_root, 0);
  return glw_video_newframe_blend(gv, vd, flags, &surface_release, 0);
}

static int
bind_p010_surface(glw_video_t *gv, glw_video_surface_t *gvs)
{
  if(gvs->gvs_uploaded)
    return 0;

  CVPixelBufferRef pb = gvs->gvs_opaque;
  p010_aux_t *aux = gv->gv_aux;
  const float headroom = osx_get_edr_headroom(gv->w.glw_root);

  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pb);
  const int nv12 = pixel_format ==
                     kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                   pixel_format ==
                     kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
  CVPixelBufferRef metal = osx_metal_convert_yuv(pb, gvs->gvs_format,
                                                  gvs->gvs_hdr_peak_luminance,
                                                  headroom);
  if(metal != NULL) {
    IOSurfaceRef rgba_surface = CVPixelBufferGetIOSurface(metal);
    CGLContextObj rgba_ctx = CGLGetCurrentContext();
    if(rgba_surface != NULL && rgba_ctx != NULL) {
      if(gvs->gvs_texture.textures[0] == 0)
        glGenTextures(2, gvs->gvs_texture.textures);
      glBindTexture(GL_TEXTURE_RECTANGLE_ARB, gvs->gvs_texture.textures[0]);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      CGLError rgba_err = CGLTexImageIOSurface2D(
        rgba_ctx, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA16F_ARB,
        gvs->gvs_width[0], gvs->gvs_height[0], GL_RGBA, GL_HALF_FLOAT,
        rgba_surface, 0);
      if(rgba_err == kCGLNoError) {
        CFRelease(pb);
        gvs->gvs_opaque = metal;
        gvs->gvs_texture.gltype = GL_TEXTURE_RECTANGLE_ARB;
        gvs->gvs_texture.width = gvs->gvs_width[0];
        gvs->gvs_texture.height = gvs->gvs_height[0];
        gvs->gvs_uploaded = 2;
        if(!aux->metal_reported) {
          TRACE(TRACE_INFO, "GLW",
                "%s IOSurface converted by Metal into an RGBA16F EDR surface",
                nv12 ? "NV12 HDR" : "P010");
          aux->metal_reported = 1;
        }
        return 0;
      }
      TRACE(TRACE_ERROR, "GLW", "Metal RGBA16F IOSurface import failed: %s",
            CGLErrorString(rgba_err));
    }
    CFRelease(metal);
  }

  if(!aux->metal_failed_reported) {
    TRACE(TRACE_INFO, "GLW",
          "Metal %s conversion unavailable; retaining direct OpenGL fallback",
          nv12 ? "NV12 HDR" : "P010");
    aux->metal_failed_reported = 1;
  }
  IOSurfaceRef surface = CVPixelBufferGetIOSurface(pb);
  CGLContextObj ctx = CGLGetCurrentContext();
  if(surface == NULL || ctx == NULL)
    return -1;

  if(gvs->gvs_texture.textures[0] == 0)
    glGenTextures(2, gvs->gvs_texture.textures);

  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, gvs->gvs_texture.textures[0]);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  CGLError err = CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE_ARB,
                                        nv12 ? GL_LUMINANCE8 : GL_LUMINANCE16,
                                        gvs->gvs_width[0], gvs->gvs_height[0],
                                        GL_LUMINANCE,
                                        nv12 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT,
                                        surface, 0);
  if(err != kCGLNoError) {
    TRACE(TRACE_ERROR, "GLW", "P010 luma IOSurface import failed: %s",
          CGLErrorString(err));
    return -1;
  }

  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, gvs->gvs_texture.textures[1]);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  err = CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE_ARB,
                               nv12 ? GL_RG8 : GL_RG16,
                               gvs->gvs_width[1], gvs->gvs_height[1],
                               GL_RG,
                               nv12 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT,
                               surface, 1);
  if(err != kCGLNoError) {
    TRACE(TRACE_ERROR, "GLW", "P010 chroma IOSurface import failed: %s",
          CGLErrorString(err));
    return -1;
  }

  gvs->gvs_texture.gltype = GL_TEXTURE_RECTANGLE_ARB;
  gvs->gvs_texture.width = gvs->gvs_width[0];
  gvs->gvs_texture.height = gvs->gvs_height[0];
  gvs->gvs_uploaded = 1;
  if(!aux->import_reported) {
    TRACE(TRACE_INFO, "GLW",
          "P010 IOSurface planes imported directly into OpenGL textures");
    aux->import_reported = 1;
  }
  return 0;
}

static void
p010_render(glw_video_t *gv, glw_rctx_t *rc)
{
  glw_video_surface_t *sa = gv->gv_sa;
  glw_root_t *gr = gv->w.glw_root;
  if(sa == NULL || bind_p010_surface(gv, sa))
    return;

  gv->gv_width = sa->gvs_width[0];
  gv->gv_height = sa->gvs_height[0];
  const float edr_headroom = osx_get_edr_headroom(gr);
  const int use_edr = edr_headroom > 1.0f;
  if(!use_edr && (sa->gvs_format == AVCOL_TRC_SMPTE2084 ||
                  sa->gvs_format == AVCOL_TRC_ARIB_STD_B67))
    prop_set_string(gv->gv_mp->mp_video.mq_prop_decode_mode, "HW+TM");
  gv->gv_gpa.gpa_edr_headroom = edr_headroom;
  gv->gv_gpa.gpa_hdr_peak_luminance = sa->gvs_hdr_peak_luminance;

  glw_renderer_vtx_st(&gv->gv_quad, 0, 0, sa->gvs_height[0]);
  glw_renderer_vtx_st(&gv->gv_quad, 1, sa->gvs_width[0], sa->gvs_height[0]);
  glw_renderer_vtx_st(&gv->gv_quad, 2, sa->gvs_width[0], 0);
  glw_renderer_vtx_st(&gv->gv_quad, 3, 0, 0);

  if(sa->gvs_uploaded == 2)
    gv->gv_gpa.gpa_prog = gr->gr_be.gbr_p010_metal_1f;
  else if(sa->gvs_format == AVCOL_TRC_SMPTE2084)
    gv->gv_gpa.gpa_prog = use_edr ? gr->gr_be.gbr_p010_pq_edr_1f :
                                    gr->gr_be.gbr_p010_pq_1f;
  else if(sa->gvs_format == AVCOL_TRC_ARIB_STD_B67)
    gv->gv_gpa.gpa_prog = use_edr ? gr->gr_be.gbr_p010_hlg_edr_1f :
                                    gr->gr_be.gbr_p010_hlg_1f;
  else
    gv->gv_gpa.gpa_prog = gr->gr_be.gbr_p010_1f;

  p010_aux_t *aux = gv->gv_aux;
  const int shader_id = sa->gvs_format | (use_edr ? 0x10000 : 0) |
                        (sa->gvs_uploaded == 2 ? 0x20000 : 0);
  if(aux->shader_transfer_reported != shader_id ||
     fabsf(aux->reported_headroom - edr_headroom) >= 0.25f) {
    TRACE(TRACE_INFO, "GLW", "P010 shader selected: %s (transfer=%d, EDR headroom=%.2f, HDR peak=%.0f nits)",
          sa->gvs_uploaded == 2 ? "Metal RGBA16F EDR" :
          sa->gvs_format == AVCOL_TRC_SMPTE2084 ?
            (use_edr ? "HDR10/PQ native EDR" : "HDR10/PQ to SDR") :
          sa->gvs_format == AVCOL_TRC_ARIB_STD_B67 ?
            (use_edr ? "HLG native EDR" : "HLG to SDR") :
                                                     "BT.709 SDR",
          sa->gvs_format, edr_headroom, sa->gvs_hdr_peak_luminance);
    aux->shader_transfer_reported = shader_id;
    aux->reported_headroom = edr_headroom;
  }
  glw_renderer_draw(&gv->gv_quad, gr, rc, &sa->gvs_texture, NULL,
                    NULL, NULL, rc->rc_alpha * gv->w.glw_alpha,
                    0, &gv->gv_gpa);
}

static int
p010_deliver(const frame_info_t *fi, glw_video_t *gv,
             glw_video_engine_t *gve)
{
  CVPixelBufferRef pb = (CVPixelBufferRef)fi->fi_data[0];
  glw_video_surface_t *s;

  if(glw_video_configure(gv, gve))
    return -1;

  p010_aux_t *aux = gv->gv_aux;

  /* A seek changes the decoder epoch.  Do not let decoded frames from the
   * previous epoch occupy the small zero-copy IOSurface pool while the new
   * epoch waits for a free slot.  The currently displayed surface is owned by
   * the UI thread and will be retired by p010_newframe(). */
  if(!aux->decoder_epoch_valid || aux->decoder_epoch != fi->fi_epoch) {
    glw_video_surface_t *stale;
    glw_video_surface_t *next;
    unsigned int flushed = 0;
    for(stale = TAILQ_FIRST(&gv->gv_decoded_queue); stale != NULL;
        stale = next) {
      next = TAILQ_NEXT(stale, gvs_link);
      /* The currently displayed surfaces can remain linked on the decoded
       * queue.  Let the UI retire those after it advances to the new epoch. */
      if(stale != gv->gv_sa && stale != gv->gv_sb) {
        surface_release(gv, stale, &gv->gv_decoded_queue);
        flushed++;
      }
    }
    aux->decoder_epoch = fi->fi_epoch;
    aux->decoder_epoch_valid = 1;
    if(flushed != 0)
      TRACE(TRACE_INFO, "GLW",
            "Direct P010 seek flushed %u stale queued frame(s) for epoch %d",
            flushed, fi->fi_epoch);
  }

  /* Preserve normal decoder backpressure.  Consuming or recycling frames
   * when this small zero-copy pool is full drains the media queue faster than
   * playback, which makes HLS enter buffering and pins the displayed frame. */
  if((s = glw_video_get_surface(gv, NULL, NULL)) == NULL)
    return -1;

  CFRetain(pb);
  s->gvs_opaque = pb;
  s->gvs_width[0] = fi->fi_width;
  s->gvs_height[0] = fi->fi_height;
  s->gvs_width[1] = fi->fi_width >> 1;
  s->gvs_height[1] = fi->fi_height >> 1;
  s->gvs_format = fi->fi_color_transfer;
  s->gvs_hdr_peak_luminance = fi->fi_hdr_peak_luminance;
  s->gvs_uploaded = 0;
  glw_video_put_surface(gv, s, fi->fi_pts, fi->fi_epoch,
                        fi->fi_duration, 0, 0);
  return 0;
}

static glw_video_engine_t glw_video_p010 = {
  .gve_type = 'P010',
  .gve_init_on_ui_thread = 1,
  .gve_newframe = p010_newframe,
  .gve_render = p010_render,
  .gve_reset = p010_reset,
  .gve_init = p010_init,
  .gve_deliver = p010_deliver,
};

GLW_REGISTER_GVE(glw_video_p010);

/* 8-bit H.264 may legitimately carry BT.2020 HLG. It uses NV12 rather than
 * P010, but shares the same timestamp-safe IOSurface/Metal EDR renderer. */
static glw_video_engine_t glw_video_nv12_hdr = {
  .gve_type = 'NVHE',
  .gve_init_on_ui_thread = 1,
  .gve_newframe = p010_newframe,
  .gve_render = p010_render,
  .gve_reset = p010_reset,
  .gve_init = p010_init,
  .gve_deliver = p010_deliver,
};

GLW_REGISTER_GVE(glw_video_nv12_hdr);

#endif
