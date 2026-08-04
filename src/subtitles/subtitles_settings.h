/**
 *
 */
struct subtitle_settings {

  struct setting *scaling_setting;
  struct setting *align_on_video_setting;
  struct setting *vertical_displacement_setting;
  struct setting *horizontal_displacement_setting;

  int alignment;   // LAYOUT_ALIGN_ from layout.h
  int style_override;
  int color;
  int shadow_color;
  int shadow_displacement;
  int outline_color;
  int outline_size;
  int bounding_box;
  int sdh_override;
  int subtile_scale;
  int drm_scale;
  int vdisplace;
  
  //int provider_embedded;
  
};

extern struct subtitle_settings subtitle_settings;
