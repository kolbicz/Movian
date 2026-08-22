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
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "glw_video_common.h"

#include <CoreVideo/CVOpenGLESTextureCache.h>
#include <CoreVideo/CVPixelBuffer.h>

#define NUM_SURFACES 4

#include "video/video_decoder.h"
#include "video/video_playback.h"

typedef struct gvv_aux {
  CVOpenGLESTextureCacheRef gvv_tex_cache_ref;
  unsigned int gvv_late_frames_dropped;
  int gvv_native_enabled;
  int gvv_native_failed_reported;
  int gvv_native_epoch;
  int gvv_native_epoch_valid;
  int gvv_decoder_epoch;
  int gvv_decoder_epoch_valid;
} gvv_aux_t;


typedef struct reap_task {
  glw_video_reap_task_t hdr;
  GLuint tex[2];
  void *data[2];
  void *opaque;
} reap_task_t;

extern CVEAGLContext ios_get_gles_context(glw_root_t *gr);
extern CVPixelBufferRef ios_metal_convert_p010(CVPixelBufferRef source,
                                               int transfer, float hdr_peak);
extern int ios_native_p010_available(glw_root_t *gr);
extern int ios_native_p010_present(CVPixelBufferRef image, int transfer,
                                   float hdr_peak);
extern void ios_native_p010_flush(void);

/**
 *
 */
static void
do_reap(glw_video_t *gv, reap_task_t *t)
{
  if(t->tex[0] != 0)
    glDeleteTextures(2, t->tex);


  for(int i = 0; i < 2; i++) {
    if(t->data[i] != NULL) {
      CFRelease(t->data[i]);
    }
  }

  if(t->opaque != NULL) {
    CFRelease(t->opaque);
  }

}


/**
 *
 */
static void
surface_release_buffers(glw_video_surface_t *gvs)
{
  for(int i = 0; i < 2; i++) {
    if(gvs->gvs_data[i] != NULL) {
      CFRelease(gvs->gvs_data[i]);
      gvs->gvs_data[i] = NULL;
    }
  }
  if(gvs->gvs_opaque != NULL) {
    CFRelease(gvs->gvs_opaque);
    gvs->gvs_opaque = NULL;
  }
}


/**
 *
 */
static void
surface_reset(glw_video_t *gv, glw_video_surface_t *gvs)
{
  reap_task_t *t = glw_video_add_reap_task(gv, sizeof(reap_task_t), do_reap);
  t->tex[0] = gvs->gvs_texture.textures[0];
  t->tex[1] = gvs->gvs_texture.textures[1];

  t->opaque = gvs->gvs_opaque;
  gvs->gvs_opaque = NULL;
  
  t->data[0] = gvs->gvs_data[0];
  t->data[1] = gvs->gvs_data[1];
  
  gvs->gvs_data[0] = NULL;
  gvs->gvs_data[1] = NULL;
}


/**
 *
 */
static void
gvv_reset(glw_video_t *gv)
{
  for(int i = 0; i < GLW_VIDEO_MAX_SURFACES; i++)
    surface_reset(gv, &gv->gv_surfaces[i]);
  
  gvv_aux_t *gvv = gv->gv_aux;
  
  if(gvv == NULL)
    return;
  
  gv->gv_aux = NULL;
  
  CFRelease(gvv->gvv_tex_cache_ref);
  free(gvv);
}


/**
 *
 */
static void
surface_init(glw_video_t *gv, glw_video_surface_t *gvs)
{
  TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  hts_cond_signal(&gv->gv_avail_queue_cond);
}

/**
 *
 */
static void
make_surfaces_available(glw_video_t *gv)
{
  for(int i = 0; i < NUM_SURFACES; i++) {
    glw_video_surface_t *gvs = &gv->gv_surfaces[i];
    TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  }
}


/**
 *
 */
static void
surface_release(glw_video_t *gv, glw_video_surface_t *gvs,
                struct glw_video_surface_queue *fromqueue)
{
  assert(gvs != gv->gv_sa);
  assert(gvs != gv->gv_sb);
  
  surface_release_buffers(gvs);
  
  TAILQ_REMOVE(fromqueue, gvs, gvs_link);
  
  TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  hts_cond_signal(&gv->gv_avail_queue_cond);
}


static void
load_texture(glw_root_t *gr, glw_program_t *gp, void *aux,
             const glw_backend_texture_t *b, int num)

{
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, b->textures[1]);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, b->textures[0]);
}



static const float cmatrix_ITUR_BT_601[16] = {
  1.164400,   1.164400, 1.164400, 0,
  0.000000,  -0.391800, 2.017200, 0,
  1.596000,  -0.813000, 0.000000, 0,
  -0.874190,   0.531702,-1.085616, 1
};

static const float cmatrix_ITUR_BT_709[16] = {
  1.164400,  1.164400,  1.164400, 0,
  0.000000, -0.213200,  2.112400, 0,
  1.792700, -0.532900,  0.000000, 0,
  -0.972926,  0.301453, -1.133402, 1
};

static const float cmatrix_SMPTE_240M[16] = {
  1.164400,  1.164400,  1.164400, 0,
  0.000000, -0.257800,  2.078700, 0,
  1.793900, -0.542500,  0.000000, 0,
  -0.973528,  0.328659, -1.116486, 1
};



/**
 *
 */
static void
gv_color_matrix_set(glw_video_t *gv, const struct frame_info *fi)
{
  const float *f;
  
  switch(fi->fi_color_space) {
    case COLOR_SPACE_BT_709:
      f = cmatrix_ITUR_BT_709;
      break;
      
    case COLOR_SPACE_BT_601:
      f = cmatrix_ITUR_BT_601;
      break;
      
    case COLOR_SPACE_SMPTE_240M:
      f = cmatrix_SMPTE_240M;
      break;
      
    default:
      f = fi->fi_height < 720 ? cmatrix_ITUR_BT_601 : cmatrix_ITUR_BT_709;
      break;
  }
  
  memcpy(gv->gv_cmatrix_tgt, f, sizeof(float) * 16);
}


static void
gv_color_matrix_update(glw_video_t *gv)
{
  int i;
  for(i = 0; i < 16; i++)
    gv->gv_cmatrix_cur[i] = (gv->gv_cmatrix_cur[i] * 3.0f +
                             gv->gv_cmatrix_tgt[i]) / 4.0f;
}


/**
 *
 */
static int
gvv_init(glw_video_t *gv)
{
  glw_root_t *gr = gv->w.glw_root;
  
  gvv_aux_t *gvv = calloc(1, sizeof(gvv_aux_t));
  gv->gv_aux = gvv;
  
  gv->gv_gpa.gpa_aux = gv;
  gv->gv_gpa.gpa_load_uniforms = glw_video_opengl_load_uniforms;
  gv->gv_gpa.gpa_load_texture = load_texture;
  
  CVOpenGLESTextureCacheCreate(NULL, NULL, gr->gr_private, NULL, &gvv->gvv_tex_cache_ref);
  
  /* This initializer is shared by the CVPixelBuffer texture-cache renderer
   * and the P010 byte-upload fallback.  Only the former is zero-copy, so do
   * not make a transport claim here.  The selected engine logs its actual
   * upload/import path separately. */
  TRACE(TRACE_DEBUG, "GLW",
        "Initialized OpenGL ES video texture renderer");
  
  make_surfaces_available(gv);
  return 0;
}



/**
 *
 */
static int64_t
gvv_newframe(glw_video_t *gv, video_decoder_t *vd, int flags)
{
  hts_mutex_assert(&gv->gv_surface_mutex);
  
  glw_video_surface_t *gvs;
  
  while((gvs = TAILQ_FIRST(&gv->gv_parked_queue)) != NULL) {
    TAILQ_REMOVE(&gv->gv_parked_queue, gvs, gvs_link);
    surface_init(gv, gvs);
  }
  
  glw_need_refresh(gv->w.glw_root, 0);
  gv_color_matrix_update(gv);

  return glw_video_newframe_blend(gv, vd, flags, &surface_release, 0);
}


/**
 *
 */
static void
upload_texture(glw_video_t *gv, glw_video_surface_t *gvs)
{
  CVReturn r;
  gvv_aux_t *gvv = gv->gv_aux;
  if(gvs->gvs_opaque == NULL)
    return;
  
  
  CVOpenGLESTextureCacheFlush(gvv->gvv_tex_cache_ref, 0);
  
  CVImageBufferRef img = gvs->gvs_opaque;
  
 
  r = CVOpenGLESTextureCacheCreateTextureFromImage(NULL,
                                                   gvv->gvv_tex_cache_ref,
                                                   img,
                                                   NULL,
                                                   GL_TEXTURE_2D, GL_LUMINANCE,
                                                   gvs->gvs_width[0], gvs->gvs_height[0],
                                                   GL_LUMINANCE, GL_UNSIGNED_BYTE, 0,
                                                   (CVOpenGLESTextureRef *)&gvs->gvs_data[0]);
  
  if(r) {
    printf("fail %d\n", r);
    return;
  }
  
  r = CVOpenGLESTextureCacheCreateTextureFromImage(NULL,
                                                   gvv->gvv_tex_cache_ref,
                                                   img,
                                                   NULL,
                                                   GL_TEXTURE_2D, GL_LUMINANCE_ALPHA,
                                                   gvs->gvs_width[1], gvs->gvs_height[1],
                                                   GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 1,
                                                   (CVOpenGLESTextureRef *)&gvs->gvs_data[1]);

  if(r) {
    printf("fail %d\n", r);
    return;
  }

  gvs->gvs_texture.gltype      = GL_TEXTURE_2D;
  gvs->gvs_texture.textures[0] = CVOpenGLESTextureGetName(gvs->gvs_data[0]);
  gvs->gvs_texture.textures[1] = CVOpenGLESTextureGetName(gvs->gvs_data[1]);
  
  glBindTexture(GL_TEXTURE_2D, gvs->gvs_texture.textures[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  
  glBindTexture(GL_TEXTURE_2D, gvs->gvs_texture.textures[1]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  
  
  
  
  gvs->gvs_texture.width  = gvs->gvs_width[0];
  gvs->gvs_texture.height = gvs->gvs_height[0];

  CFRelease(gvs->gvs_opaque);
  gvs->gvs_opaque = NULL;
}


/**
 *
 */
static void
gvv_render(glw_video_t *gv, glw_rctx_t *rc)
{
  glw_video_surface_t *sa = gv->gv_sa;
  glw_video_surface_t *sb = NULL;
  glw_root_t *gr = gv->w.glw_root;
  glw_program_t *gp;
  glw_backend_root_t *gbr = &gr->gr_be;
  
  if(sa == NULL)
    return;

  gvv_aux_t *gvv = gv->gv_aux;
  const int hdr = sa->gvs_format == AVCOL_TRC_SMPTE2084 ||
                  sa->gvs_format == AVCOL_TRC_ARIB_STD_B67;
  if(hdr && gvv != NULL) {
    if(!gvv->gvv_native_enabled)
      gvv->gvv_native_enabled = ios_native_p010_available(gv->w.glw_root);
    if(gvv->gvv_native_enabled) {
      if(!gvv->gvv_native_epoch_valid ||
         gvv->gvv_native_epoch != sa->gvs_epoch) {
        ios_native_p010_flush();
        gvv->gvv_native_epoch = sa->gvs_epoch;
        gvv->gvv_native_epoch_valid = 1;
      }
      if(sa->gvs_uploaded == 3)
        return;
      const int native_result = sa->gvs_opaque != NULL ?
        ios_native_p010_present(sa->gvs_opaque, sa->gvs_format,
                                sa->gvs_hdr_peak_luminance) : -1;
      if(native_result > 0) {
        sa->gvs_uploaded = 3;
        return;
      }
      if(native_result == 0)
        return;
      gvv->gvv_native_enabled = 0;
      ios_native_p010_flush();
      prop_set_string(gv->gv_mp->mp_video.mq_prop_decode_mode, "HW+TM");
      if(!gvv->gvv_native_failed_reported) {
        gvv->gvv_native_failed_reported = 1;
        TRACE(TRACE_INFO, "GLW",
              "Native NV12 HDR output unavailable; using SDR compatibility renderer");
      }
    }
  }
  
  gv->gv_width  = sa->gvs_width[0];
  gv->gv_height = sa->gvs_height[0];
  
  upload_texture(gv, sa);
  
  glw_renderer_vtx_st(&gv->gv_quad,  0, 0, 1);
  glw_renderer_vtx_st(&gv->gv_quad,  1, 1, 1);
  glw_renderer_vtx_st(&gv->gv_quad,  2, 1, 0);
  glw_renderer_vtx_st(&gv->gv_quad,  3, 0, 0);
  
  gp = gbr->gbr_yc2rgb_1f;
  gv->gv_gpa.gpa_prog = gp;

  glw_renderer_draw(&gv->gv_quad, gr, rc,
                    &sa->gvs_texture,
                    sb != NULL ? &sb->gvs_texture : NULL,
                    NULL, NULL,
                    rc->rc_alpha * gv->w.glw_alpha, 0, &gv->gv_gpa);
}

/**
 *
 */
static int
gvv_deliver(const frame_info_t *fi, glw_video_t *gv, glw_video_engine_t *gve)
{
  CVImageBufferRef img = (CVImageBufferRef)fi->fi_data[0];
  glw_video_surface_t *s;
  
  glw_video_configure(gv, gve);
  
  if((s = glw_video_get_surface(gv, NULL, NULL)) == NULL)
    return -1;
  
  gv_color_matrix_set(gv, fi);

  assert(s->gvs_opaque == NULL);
  
  CFRetain(img);
  s->gvs_opaque = img;
  s->gvs_uploaded = 0;
  s->gvs_width[0] = fi->fi_width;
  s->gvs_height[0] = fi->fi_height;
  s->gvs_width[1] = fi->fi_width >> 1;
  s->gvs_height[1] = fi->fi_height >> 1;
  s->gvs_width[2] = fi->fi_width >> 1;
  s->gvs_height[2] = fi->fi_height >> 1;
  s->gvs_format = fi->fi_color_transfer;
  s->gvs_hdr_peak_luminance = fi->fi_hdr_peak_luminance;
  glw_video_put_surface(gv, s, fi->fi_pts, fi->fi_epoch, fi->fi_duration, 0, 0);
  return 0;
}

static void
cvpb_reset(glw_video_t *gv)
{
  ios_native_p010_flush();
  gvv_reset(gv);
}


/**
 *
 */
static glw_video_engine_t glw_video_cvpb = {
  .gve_type = 'CVPB',
  .gve_init_on_ui_thread = 1,
  .gve_newframe = gvv_newframe,
  .gve_render   = gvv_render,
  .gve_reset    = cvpb_reset,
  .gve_init     = gvv_init,
  .gve_deliver  = gvv_deliver,
};

GLW_REGISTER_GVE(glw_video_cvpb);


/*
 * OpenGL ES 2 has no portable normalized 16-bit RG texture format.  Import
 * P010 as byte-packed LA/RGBA textures and reconstruct each 10-bit component
 * in the fragment shader.  This avoids the 10-to-8-bit pixel-transfer stage;
 * a later Metal renderer can make this path zero-copy.
 */
static int
p010_ios_init(glw_video_t *gv)
{
  int r = gvv_init(gv);
  gvv_aux_t *gvv = gv->gv_aux;
  gvv->gvv_native_enabled = ios_native_p010_available(gv->w.glw_root);
  memcpy(gv->gv_cmatrix_cur, cmatrix_ITUR_BT_709,
         sizeof(gv->gv_cmatrix_cur));
  memcpy(gv->gv_cmatrix_tgt, cmatrix_ITUR_BT_709,
         sizeof(gv->gv_cmatrix_tgt));
  TRACE(TRACE_INFO, "GLW", "%s",
        gvv->gvv_native_enabled ?
        "P010 renderer initialized (native HDR display layer)" :
        "P010 renderer initialized (Metal GPU bridge with OpenGL ES fallback)");
  return r;
}

static void
p010_ios_reset(glw_video_t *gv)
{
  ios_native_p010_flush();
  gvv_reset(gv);
}

static int
p010_ios_upload(glw_video_t *gv, glw_video_surface_t *gvs)
{
  if(gvs->gvs_opaque == NULL)
    return 0;

  CVPixelBufferRef pb = gvs->gvs_opaque;
  gvv_aux_t *gvv = gv->gv_aux;

  /* Metal can import the decoder's R16/RG16 P010 planes without touching the
   * CPU.  Its IOSurface-backed BGRA result can then be imported into the
   * existing OpenGL ES compositor, preserving subtitles, OSD and menus. */
  CVPixelBufferRef converted = ios_metal_convert_p010(
    pb, gvs->gvs_format, gvs->gvs_hdr_peak_luminance);
  if(converted != NULL) {
    CVOpenGLESTextureCacheFlush(gvv->gvv_tex_cache_ref, 0);
    CVOpenGLESTextureRef texture = NULL;
    CVReturn r = CVOpenGLESTextureCacheCreateTextureFromImage(
      NULL, gvv->gvv_tex_cache_ref, converted, NULL,
      GL_TEXTURE_2D, GL_RGBA, gvs->gvs_width[0], gvs->gvs_height[0],
      GL_BGRA, GL_UNSIGNED_BYTE, 0, &texture);
    CVPixelBufferRelease(converted);
    if(r == kCVReturnSuccess && texture != NULL) {
      gvs->gvs_data[0] = texture;
      gvs->gvs_texture.gltype = GL_TEXTURE_2D;
      gvs->gvs_texture.textures[0] = CVOpenGLESTextureGetName(texture);
      gvs->gvs_texture.textures[1] = 0;
      gvs->gvs_texture.width = gvs->gvs_width[0];
      gvs->gvs_texture.height = gvs->gvs_height[0];
      glBindTexture(GL_TEXTURE_2D, gvs->gvs_texture.textures[0]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gvs->gvs_uploaded = 2;
      CFRelease(gvs->gvs_opaque);
      gvs->gvs_opaque = NULL;
      static int logged;
      if(!logged) {
        logged = 1;
        TRACE(TRACE_INFO, "GLW",
              "P010 IOSurface converted by Metal GPU and imported into OpenGL ES");
      }
      return 0;
    }
    if(texture != NULL)
      CFRelease(texture);
  }

  gvs->gvs_uploaded = 1;
  CVReturn status = CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  if(status != kCVReturnSuccess)
    return -1;

  if(gvs->gvs_texture.textures[0] == 0)
    glGenTextures(2, gvs->gvs_texture.textures);

  GLint old_unpack_alignment;
  GLint old_active_texture;
  GLint old_texture_binding;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack_alignment);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindTexture(GL_TEXTURE_2D, gvs->gvs_texture.textures[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
               gvs->gvs_width[0], gvs->gvs_height[0], 0,
               GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
               CVPixelBufferGetBaseAddressOfPlane(pb, 0));

  glBindTexture(GL_TEXTURE_2D, gvs->gvs_texture.textures[1]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
               gvs->gvs_width[1], gvs->gvs_height[1], 0,
               GL_RGBA, GL_UNSIGNED_BYTE,
               CVPixelBufferGetBaseAddressOfPlane(pb, 1));

  GLenum err = glGetError();
  glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
  glActiveTexture(old_active_texture);
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  if(err != GL_NO_ERROR) {
    TRACE(TRACE_ERROR, "GLW", "P010 byte-packed texture upload failed: 0x%x",
          err);
    return -1;
  }

  gvs->gvs_texture.gltype = GL_TEXTURE_2D;
  gvs->gvs_texture.width = gvs->gvs_width[0];
  gvs->gvs_texture.height = gvs->gvs_height[0];
  CFRelease(gvs->gvs_opaque);
  gvs->gvs_opaque = NULL;
  return 0;
}

static void
p010_ios_render(glw_video_t *gv, glw_rctx_t *rc)
{
  glw_video_surface_t *sa = gv->gv_sa;
  if(sa == NULL)
    return;

  gvv_aux_t *gvv = gv->gv_aux;
  /* Engine changes are initiated by the decoder thread, while engines that
   * require UI-thread setup are initialized from glw_video_newframe().  A
   * render pass can occur in between those two steps, particularly when HLS
   * switches between variants with different dimensions/pixel formats. */
  if(gvv == NULL)
    return;

  if(gvv->gvv_native_enabled) {
    /* A temporarily detached/reconfiguring view (for example during
     * rotation) is not a renderer failure.  Wait for the native layer to be
     * attachable again rather than drawing the same session through GL. */
    if(!ios_native_p010_available(gv->w.glw_root))
      return;
    if(!gvv->gvv_native_epoch_valid ||
       gvv->gvv_native_epoch != sa->gvs_epoch) {
      ios_native_p010_flush();
      gvv->gvv_native_epoch = sa->gvs_epoch;
      gvv->gvv_native_epoch_valid = 1;
      TRACE(TRACE_DEBUG, "GLW",
            "Native P010 display layer flushed for video epoch %d",
            sa->gvs_epoch);
    }
    if(sa->gvs_uploaded == 3)
      return;
    int native_result = sa->gvs_opaque != NULL ?
      ios_native_p010_present(sa->gvs_opaque, sa->gvs_format,
                              sa->gvs_hdr_peak_luminance) : -1;
    if(native_result > 0) {
      sa->gvs_uploaded = 3;
      static int logged;
      if(!logged) {
        logged = 1;
        TRACE(TRACE_INFO, "GLW",
              "P010 IOSurface presented directly by AVSampleBufferDisplayLayer");
      }
      return;
    }
    if(native_result == 0)
      return;

    gvv->gvv_native_enabled = 0;
    ios_native_p010_flush();
    prop_set_string(gv->gv_mp->mp_video.mq_prop_decode_mode, "HW+TM");
    if(!gvv->gvv_native_failed_reported) {
      gvv->gvv_native_failed_reported = 1;
      if(native_result == -2)
        TRACE(TRACE_INFO, "GLW",
              "Native HDR output unavailable on this display; using SDR compatibility renderer");
      else
        TRACE(TRACE_ERROR, "GLW",
              "Native P010 display layer failed; switching session to compatibility renderer");
    }
  }

  if(p010_ios_upload(gv, sa))
    return;

  gv->gv_width = sa->gvs_width[0];
  gv->gv_height = sa->gvs_height[0];
  glw_renderer_vtx_st(&gv->gv_quad, 0, 0, 1);
  glw_renderer_vtx_st(&gv->gv_quad, 1, 1, 1);
  glw_renderer_vtx_st(&gv->gv_quad, 2, 1, 0);
  glw_renderer_vtx_st(&gv->gv_quad, 3, 0, 0);

  glw_root_t *gr = gv->w.glw_root;
  gv->gv_gpa.gpa_hdr_peak_luminance = sa->gvs_hdr_peak_luminance;
  gv->gv_gpa.gpa_prog = sa->gvs_uploaded == 2 ? gr->gr_be.gbr_rgb2rgb_1f :
    sa->gvs_format == AVCOL_TRC_SMPTE2084 ? gr->gr_be.gbr_p010_pq_1f :
    sa->gvs_format == AVCOL_TRC_ARIB_STD_B67 ? gr->gr_be.gbr_p010_hlg_1f :
                                               gr->gr_be.gbr_p010_1f;
  glw_renderer_draw(&gv->gv_quad, gr, rc, &sa->gvs_texture, NULL,
                    NULL, NULL, rc->rc_alpha * gv->w.glw_alpha,
                    0, &gv->gv_gpa);
}

static int
p010_ios_deliver(const frame_info_t *fi, glw_video_t *gv,
                 glw_video_engine_t *gve)
{
  CVPixelBufferRef pb = (CVPixelBufferRef)fi->fi_data[0];
  glw_video_surface_t *s;
  glw_video_configure(gv, gve);

  /* The common renderer can discard stale queued surfaces, but on the P010
   * path that only happens after a frame has occupied one of four surfaces.
   * Following a seek, audio can therefore run ahead while the decoder blocks
   * behind frames that can no longer be presented.  Drop clearly late frames
   * here, before retaining the pixel buffer or waiting for a surface. */
  media_pipe_t *mp = gv->gv_mp;
  int64_t aclock = PTS_UNSET;
  int audio_epoch = 0;
  hts_mutex_lock(&mp->mp_clock_mutex);
  if(mp->mp_audio_clock != PTS_UNSET && mp->mp_audio_clock_epoch != 0) {
    aclock = mp->mp_audio_clock + arch_get_avtime() -
      mp->mp_audio_clock_avtime + mp->mp_avdelta;
    audio_epoch = mp->mp_audio_clock_epoch;
  }
  hts_mutex_unlock(&mp->mp_clock_mutex);

  gvv_aux_t *gvv = gv->gv_aux;

  /* A seek changes the decoder epoch.  Retire decoded surfaces from the
   * previous epoch before taking another one of the small renderer pool.
   * Otherwise audio starts on the new epoch while stale video is presented
   * until the old queue has drained. */
  if(!gvv->gvv_decoder_epoch_valid ||
     gvv->gvv_decoder_epoch != fi->fi_epoch) {
    glw_video_surface_t *stale;
    glw_video_surface_t *next;
    unsigned int flushed = 0;
    for(stale = TAILQ_FIRST(&gv->gv_decoded_queue); stale != NULL;
        stale = next) {
      next = TAILQ_NEXT(stale, gvs_link);
      /* gv_sa/gv_sb may still reside on the decoded queue while the UI is
       * presenting them.  Their ownership ends in glw_video_newframe_blend;
       * releasing either here trips surface_release()'s invariant and can
       * hand a displayed IOSurface back to the decoder. */
      if(stale != gv->gv_sa && stale != gv->gv_sb) {
        surface_release(gv, stale, &gv->gv_decoded_queue);
        flushed++;
      }
    }
    gvv->gvv_decoder_epoch = fi->fi_epoch;
    gvv->gvv_decoder_epoch_valid = 1;
    gvv->gvv_late_frames_dropped = 0;
    if(flushed != 0)
      TRACE(TRACE_INFO, "GLW",
            "iOS P010 seek flushed %u stale queued frame(s) for epoch %d",
            flushed, fi->fi_epoch);
  }

  if(!gvv->gvv_native_enabled &&
     aclock != PTS_UNSET && audio_epoch == fi->fi_epoch &&
     fi->fi_pts != PTS_UNSET && aclock - fi->fi_pts > 250000) {
    gvv->gvv_late_frames_dropped++;
    if(gvv->gvv_late_frames_dropped == 1 ||
       !(gvv->gvv_late_frames_dropped % 60)) {
      TRACE(TRACE_INFO, "GLW",
            "P010 catch-up dropped %u late frame(s), video behind audio by %d ms",
            gvv->gvv_late_frames_dropped,
            (int)((aclock - fi->fi_pts) / 1000));
    }
    return 0;
  }

  if((s = glw_video_get_surface(gv, NULL, NULL)) == NULL)
    return -1;

  gv_color_matrix_set(gv, fi);
  CFRetain(pb);
  s->gvs_opaque = pb;
  s->gvs_uploaded = 0;
  s->gvs_width[0] = fi->fi_width;
  s->gvs_height[0] = fi->fi_height;
  s->gvs_width[1] = fi->fi_width >> 1;
  s->gvs_height[1] = fi->fi_height >> 1;
  s->gvs_format = fi->fi_color_transfer;
  s->gvs_hdr_peak_luminance = fi->fi_hdr_peak_luminance;
  glw_video_put_surface(gv, s, fi->fi_pts, fi->fi_epoch,
                        fi->fi_duration, 0, 0);
  return 0;
}

static glw_video_engine_t glw_video_p010_ios = {
  .gve_type = 'P010',
  .gve_init_on_ui_thread = 1,
  .gve_newframe = gvv_newframe,
  .gve_render = p010_ios_render,
  .gve_reset = p010_ios_reset,
  .gve_init = p010_ios_init,
  .gve_deliver = p010_ios_deliver,
};

GLW_REGISTER_GVE(glw_video_p010_ios);
